"""Historical 0.1 store implementation, retained ONLY as a manifest-compatibility test oracle."""
from __future__ import annotations

import atexit
import collections
import contextlib
import hashlib
import json
import logging
import os
from pathlib import Path, PurePosixPath
import platform
import re
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
FORMAT_VERSION = 1
MAX_ARCHIVE = 1024**3
MAX_EXPANDED = 4 * 1024**3
MAX_FRAME = 128 * 1024**2
DOCKER_IMAGE = "python:3.12-slim@sha256:a6e34c598f2467ed0e9a8d349809fcd8b5c603269512df273a0bb1784edc11b1"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def label(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,95}", value):
        raise ValueError("Invalid release identifier")
    return value


def safe_extract(archive, destination):
    """ZIPs may not escape staging, carry links, or expand without a bound."""
    with zipfile.ZipFile(archive) as z:
        entries = z.infolist()
        if len(entries) > 20000 or sum(i.file_size for i in entries) > MAX_EXPANDED:
            raise ValueError("Archive exceeds extraction limits")
        seen = set()
        for info in entries:
            p = PurePosixPath(info.filename)
            if (p.is_absolute() or ".." in p.parts or "\\" in info.filename
                    or ":" in info.filename or not p.parts):
                raise ValueError("Unsafe archive path")
            if stat.S_ISLNK(info.external_attr >> 16):
                raise ValueError("Archive symlinks are not supported")
            if str(p) in seen:
                raise ValueError("Duplicate archive entry")
            seen.add(str(p))
        z.extractall(destination)


def payload_files(root):
    result = {}
    for p in sorted(root.rglob("*")):
        if p.is_symlink():
            raise ValueError("Bundle symlinks are not supported")
        if p.is_file():
            result[p.relative_to(root).as_posix()] = sha256(p)
    return result


class Store:
    def __init__(self, root=None, catalog=None):
        self.root = Path(root or os.environ.get("GEOSUPPORT_HOME", "~/.cache/duckdb-geosupport")).expanduser().resolve()
        self.catalog_path = Path(catalog or os.environ.get("GEOSUPPORT_CATALOG", HERE / "releases.json"))

    def catalog(self):
        data = json.loads(self.catalog_path.read_text())
        if data.get("schema_version") != FORMAT_VERSION:
            raise ValueError("Unsupported catalog schema")
        return data["releases"]

    def available(self):
        return list(self.catalog().values())

    def resolve(self, name):
        label(name)
        if re.fullmatch(r"[0-9a-f]{64}", name):
            bundle_id = name
        else:
            alias = self.root / "aliases" / (name + ".json")
            if not alias.exists():
                raise ValueError(f"Release {name!r} is not installed; use gs_install or gs_import explicitly")
            bundle_id = json.loads(alias.read_text())["bundle_id"]
        if not re.fullmatch(r"[0-9a-f]{64}", bundle_id):
            raise ValueError("Invalid bundle identity")
        path = self.root / "bundles" / bundle_id
        manifest = json.loads((path / "manifest.json").read_text())
        identity = manifest["identity"]
        if hashlib.sha256(canonical(identity).encode()).hexdigest() != bundle_id:
            raise ValueError("Bundle manifest identity mismatch")
        return path, manifest

    def verify(self, name):
        path, manifest = self.resolve(name)
        identity = manifest["identity"]
        if identity["format_version"] != FORMAT_VERSION:
            raise ValueError("Unsupported bundle format")
        if payload_files(path / "payload") != identity["files"]:
            raise ValueError(f"Bundle {name} payload checksum mismatch")
        return manifest

    def installed(self):
        rows = []
        for p in sorted((self.root / "bundles").glob("*/manifest.json")):
            row = self.describe(json.loads(p.read_text()))
            row["verified"] = False  # Inventory does not hash gigabytes of payloads.
            rows.append(row)
        return rows

    @staticmethod
    def describe(manifest):
        i = manifest["identity"]
        return {"release": i["release"], "bundle_id": manifest["bundle_id"],
                "engine_version": i["engine_version"], "platform": i["platform"],
                "patches": i["patches"], "verified": True,
                "wrapper_commit": "82c39e82b4d4dc7773fa67d32ef7f3787ef4d27e",
                "extension_version": "0.1.0"}

    def install(self, name, archive=None):
        label(name)
        entry = self.catalog().get(name)
        if entry is None:
            raise ValueError(f"Release {name!r} is not in the pinned catalog")
        if (self.root / "aliases" / (name + ".json")).exists():
            manifest = self.verify(name)
            expected = [a["sha256"] for a in [entry["archive"]] + entry.get("patches", [])]
            if manifest["identity"]["archives"] != expected:
                raise ValueError("Release label already refers to different bytes; use a new release label")
            return self.describe(manifest)
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "bundles").mkdir(exist_ok=True)
        (self.root / "aliases").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".install-", dir=self.root) as tmp:
            stage = Path(tmp)
            base = stage / "base.zip"
            self.acquire(entry["archive"], base, archive)
            unpack = stage / "unpack"
            safe_extract(base, unpack)
            roots = [p.parent.parent for p in unpack.rglob("libgeo.so") if p.parent.name == "lib"]
            if len(roots) != 1 or not (roots[0] / "fls").is_dir():
                raise ValueError("Expected exactly one Linux Geosupport lib/ and fls/ installation")
            publish = stage / "publish"
            publish.mkdir()
            payload = publish / "payload"
            shutil.move(str(roots[0]), payload)
            for n, patch in enumerate(entry.get("patches", [])):
                if archive:
                    raise ValueError("Offline import of patched releases requires local patch support; import base releases only")
                patch_zip = stage / f"patch-{n}.zip"
                self.acquire(patch, patch_zip)
                patch_dir = stage / f"patch-{n}"
                safe_extract(patch_zip, patch_dir)
                for p in patch_dir.rglob("*"):
                    if p.is_file():
                        rel = p.relative_to(patch_dir)
                        target = payload / "fls" / rel
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(p, target)
            identity = {"format_version": FORMAT_VERSION, "release": name,
                        "engine_version": entry["engine_version"], "platform": entry["platform"],
                        "patches": [p["sha256"] for p in entry.get("patches", [])],
                        "archives": [a["sha256"] for a in [entry["archive"]] + entry.get("patches", [])],
                        "files": payload_files(payload)}
            bundle_id = hashlib.sha256(canonical(identity).encode()).hexdigest()
            manifest = {"bundle_id": bundle_id, "identity": identity, "sources": entry}
            (publish / "manifest.json").write_text(canonical(manifest))
            dest = self.root / "bundles" / bundle_id
            try:
                publish.rename(dest)
            except OSError:
                if not dest.exists():
                    raise
                self.verify(bundle_id)
            alias = self.root / "aliases" / (name + ".json")
            temporary_alias = stage / "alias.json"
            temporary_alias.write_text(canonical({"bundle_id": bundle_id}))
            try:
                os.link(temporary_alias, alias)
            except FileExistsError:
                if json.loads(alias.read_text())["bundle_id"] != bundle_id:
                    raise ValueError("Concurrent installation published different bytes for this release")
            return self.describe(manifest)

    @staticmethod
    def acquire(spec, destination, local=None):
        if not re.fullmatch(r"[0-9a-f]{64}", spec["sha256"]):
            raise ValueError("Archive requires a SHA-256 pin")
        if local:
            source = open(Path(local).expanduser(), "rb")
        else:
            if not spec["url"].startswith("https://"):
                raise ValueError("Downloads must use HTTPS")
            source = urllib.request.urlopen(spec["url"], timeout=60)
            if not source.geturl().startswith("https://"):
                source.close()
                raise ValueError("Insecure download redirect")
        with source, open(destination, "wb") as out:
            size = 0
            for block in iter(lambda: source.read(1024 * 1024), b""):
                size += len(block)
                if size > MAX_ARCHIVE:
                    raise ValueError("Archive exceeds download limit")
                out.write(block)
        if sha256(destination) != spec["sha256"]:
            raise ValueError("Downloaded/imported archive checksum mismatch")

