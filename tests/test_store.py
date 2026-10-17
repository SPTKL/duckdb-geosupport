import hashlib
import json
from pathlib import Path
import stat
import zipfile

import pytest

from runtime.manager import Store, safe_extract, sha256


@pytest.fixture
def package(tmp_path):
    archive = tmp_path / "bundle.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr("version-test/lib/libgeo.so", b"synthetic library, never executable")
        z.writestr("version-test/fls/PAD", b"synthetic data")
    catalog = tmp_path / "catalog.json"
    entry = {"release": "test", "engine_version": "test", "platform": "linux_amd64",
             "archive": {"url": "https://example.invalid/bundle.zip", "sha256": sha256(archive)}, "patches": []}
    catalog.write_text(json.dumps({"schema_version": 1, "releases": {"test": entry}}))
    return Store(tmp_path / "store", catalog), archive


def test_install_verify_idempotent_and_tamper_detection(package):
    store, archive = package
    first = store.install("test", archive)
    assert store.install("test", archive) == first
    assert store.verify(first["bundle_id"])["identity"]["release"] == "test"
    assert store.installed()[0]["verified"] is False
    bundle, _ = store.resolve("test")
    (bundle / "payload/fls/PAD").write_bytes(b"changed")
    with pytest.raises(ValueError, match="checksum mismatch"):
        store.verify("test")


def test_bad_hash_does_not_publish_partial_release(package):
    store, archive = package
    archive.write_bytes(b"corrupted archive")
    with pytest.raises(ValueError, match="checksum mismatch"):
        store.install("test", archive)
    assert store.installed() == []
    assert not list(store.root.glob(".install-*"))
    assert not (store.root / "aliases/test.json").exists()


def test_label_cannot_be_rebound(package):
    store, archive = package
    store.install("test", archive)
    catalog = json.loads(store.catalog_path.read_text())
    catalog["releases"]["test"]["archive"]["sha256"] = "a" * 64
    store.catalog_path.write_text(json.dumps(catalog))
    with pytest.raises(ValueError, match="different bytes"):
        store.install("test", archive)


@pytest.mark.parametrize("name", ["../escape", "/absolute", "a\\escape", "C:drive"])
def test_archive_traversal_rejected(tmp_path, name):
    archive = tmp_path / "evil.zip"
    with zipfile.ZipFile(archive, "w") as z:
        z.writestr(name, "bad")
    with pytest.raises(ValueError, match="Unsafe archive"):
        safe_extract(archive, tmp_path / "extract")


def test_archive_symlink_rejected(tmp_path):
    archive = tmp_path / "evil.zip"
    with zipfile.ZipFile(archive, "w") as z:
        i = zipfile.ZipInfo("link")
        i.create_system = 3
        i.external_attr = (stat.S_IFLNK | 0o777) << 16
        z.writestr(i, "../escape")
    with pytest.raises(ValueError, match="symlinks"):
        safe_extract(archive, tmp_path / "extract")


def test_manifest_identity_tampering(package):
    store, archive = package
    store.install("test", archive)
    path, manifest = store.resolve("test")
    manifest["identity"]["engine_version"] = "changed"
    (path / "manifest.json").write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="identity mismatch"):
        store.resolve("test")


def test_missing_release_never_installs_implicitly(package):
    store, _ = package
    with pytest.raises(ValueError, match="not installed"):
        store.resolve("test")
    assert not store.root.exists()


def test_concurrent_installs_publish_one_identity(package):
    from concurrent.futures import ThreadPoolExecutor
    store, archive = package
    with ThreadPoolExecutor(max_workers=2) as pool:
        a, b = list(pool.map(lambda _: store.install("test", archive), range(2)))
    assert a == b
    assert len(store.installed()) == 1


def test_offline_install_never_downloads(package, monkeypatch):
    store, archive = package
    def forbidden(*args, **kwargs):
        raise AssertionError("Unexpected network access")
    monkeypatch.setattr("urllib.request.urlopen", forbidden)
    store.install("test", archive)
    store.verify("test")


def test_verify_detects_extra_files(package):
    store, archive = package
    store.install("test", archive)
    path, _ = store.resolve("test")
    (path / "payload/extra").write_text("unexpected")
    with pytest.raises(ValueError, match="checksum mismatch"):
        store.verify("test")
