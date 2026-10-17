"""Private JSON-lines runtime for the native DuckDB extension (Python 3.9+)."""
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


def read_frame(stream):
    line = stream.readline(MAX_FRAME + 1)
    if not line:
        raise RuntimeError("Worker exited before responding; inspect stderr")
    if len(line) > MAX_FRAME or not line.endswith(b"\n"):
        raise RuntimeError("Invalid or oversized worker frame")
    return json.loads(line)


def write_frame(stream, data):
    value = (canonical(data) + "\n").encode()
    if len(value) > MAX_FRAME:
        raise ValueError("Batch exceeds protocol limit")
    stream.write(value)
    stream.flush()


class Worker:
    def __init__(self, path, manifest):
        self.manifest = manifest
        runtime = os.environ.get("GEOSUPPORT_RUNTIME", "local")
        env = os.environ.copy()
        if runtime == "docker":
            command = [os.environ.get("GEOSUPPORT_DOCKER", "docker"), "run", "--rm", "-i",
                       "--pull=never", "--platform", "linux/amd64", "--network=none", "--read-only",
                       "--mount", f"type=bind,src={HERE},dst=/runtime,readonly",
                       "--mount", f"type=bind,src={path},dst=/bundle,readonly",
                       "-e", "GEOFILES=/bundle/payload/fls/", "-e", "LD_LIBRARY_PATH=/bundle/payload/lib",
                       "-e", "PYTHONDONTWRITEBYTECODE=1", DOCKER_IMAGE,
                       "python", "-u", "/runtime/manager.py", "--engine", "/bundle"]
        elif runtime == "local":
            if sys.platform != "linux" or platform.machine() not in ("x86_64", "AMD64"):
                raise RuntimeError("Local Geosupport requires Linux x86-64; on macOS set GEOSUPPORT_RUNTIME=docker and pre-pull the documented image")
            env["GEOFILES"] = str(path / "payload/fls") + "/"
            env["LD_LIBRARY_PATH"] = str(path / "payload/lib")
            env["PYTHONDONTWRITEBYTECODE"] = "1"
            command = [sys.executable, "-u", str(HERE / "manager.py"), "--engine", str(path)]
        else:
            raise ValueError("GEOSUPPORT_RUNTIME must be local or docker")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        try:
            ready = read_frame(self.process.stdout)
            if ready.get("ready") != manifest["bundle_id"]:
                raise RuntimeError(f"Worker startup failed: {ready}")
        except BaseException:
            self.close()
            raise

    def call(self, requests):
        write_frame(self.process.stdin, requests)
        response = read_frame(self.process.stdout)
        if "error" in response:
            raise RuntimeError(response["error"])
        return response["results"]

    def close(self):
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.process.stdout:
            self.process.stdout.close()


class Manager:
    def __init__(self):
        self.store = Store()
        self.workers = {}

    def close(self):
        for worker in self.workers.values():
            worker.close()
        self.workers.clear()

    def load(self, release):
        path, manifest = self.store.resolve(release)
        bundle_id = manifest["bundle_id"]
        if bundle_id not in self.workers:
            self.store.verify(bundle_id)
            if manifest["identity"]["platform"] != "linux_amd64":
                raise ValueError("Unsupported bundle platform")
            self.workers[bundle_id] = Worker(path, manifest)
        return self.workers[bundle_id]

    def dispatch(self, request):
        op = request["op"]
        args = request.get("args", [])
        if op == "available":
            return self.store.available()
        if op == "installed":
            return self.store.installed()
        if op in ("install", "import"):
            return [self.store.install(*args)]
        if op == "verify":
            return [self.store.describe(self.store.verify(*args))]
        if op == "load":
            return [self.store.describe(self.load(*args).manifest)]
        if op == "unload":
            _, manifest = self.store.resolve(*args)
            worker = self.workers.pop(manifest["bundle_id"], None)
            if worker:
                worker.close()
            return [self.store.describe(manifest)]
        if op == "runtime_info":
            return [self.store.describe(w.manifest) for w in self.workers.values()]
        if op == "batch":
            # Resolve each distinct release once per chunk, never once per row.
            groups = collections.defaultdict(list)
            for index, item in enumerate(request["rows"]):
                if item is not None:
                    groups[item["release"]].append((index, item))
            results = [None] * len(request["rows"])
            for release, items in groups.items():
                worker = self.load(release)
                try:
                    out = worker.call([item for _, item in items])
                except BaseException:
                    self.workers.pop(worker.manifest["bundle_id"], None)
                    worker.close()
                    raise
                if len(out) != len(items):
                    raise RuntimeError("Worker returned wrong row count")
                for (index, _), value in zip(items, out):
                    results[index] = value
            return results
        raise ValueError(f"Unknown operation {op}")


def engine_main(path):
    # Reserve stdout exclusively for protocol; the native library may print diagnostics.
    protocol = os.fdopen(os.dup(1), "wb", buffering=0)
    os.dup2(2, 1)
    sys.path.insert(0, str(HERE / "vendor"))
    from geosupport import Geosupport, GeosupportError
    from geosupport.function_info import FUNCTIONS, WORK_AREA_LAYOUTS
    from geosupport.io import get_formatter
    logging.getLogger("geosupport.geosupport").disabled = True
    manifest = json.loads((path / "manifest.json").read_text())
    identity = manifest["identity"]
    g = Geosupport()
    cache = collections.OrderedDict()
    cache_bytes = 0
    write_frame(protocol, {"ready": manifest["bundle_id"]})
    for line in sys.stdin.buffer:
        try:
            requests = json.loads(line)
            output = []
            for req in requests:
                function = req["function"].strip().upper()
                if function not in FUNCTIONS:
                    raise ValueError(f"Unsupported function: {function}")
                function = FUNCTIONS[function]["function"]
                params = req["params"]
                if not isinstance(params, dict) or "function" in params:
                    raise ValueError("Inputs must be an object; specify function separately")
                mode = req.get("mode", "regular")
                if mode not in ("regular", "extended", "long", "tpad", "long+tpad", "extended+tpad"):
                    raise ValueError(f"Unsupported mode: {mode}")
                # Fail visibly instead of truncating fields or breaking fixed-byte layouts.
                layout = WORK_AREA_LAYOUTS["input"]["WA1"]
                for key, value in params.items():
                    if key not in layout:
                        raise ValueError(f"Unknown input field: {key}")
                    if value is not None and not isinstance(value, (str, int, bool)):
                        raise ValueError(f"Input {key} must be a string, integer, boolean or null")
                    formatted = str(get_formatter(layout[key]["formatter"])(value))
                    if not formatted.isascii() or "\x00" in formatted:
                        raise ValueError(f"Input {key} must be ASCII without NUL characters")
                    if len(formatted) > layout[key]["i"][1] - layout[key]["i"][0]:
                        raise ValueError(f"Input {key} exceeds Geosupport field width")
                key = canonical([function, params, mode])
                if key in cache:
                    cache.move_to_end(key)
                    result = cache[key][0]
                else:
                    try:
                        # Wrapper set_mode doesn't recognize extended+tpad as extended.
                        actual = dict(params)
                        if mode == "extended+tpad":
                            actual["tpad"] = True
                        raw = g[function](actual, mode="extended" if mode == "extended+tpad" else mode)
                    except GeosupportError as error:
                        if not error.result:
                            raise
                        raw = error.result
                    result = summarize(raw, manifest)
                    size = len(key) + len(canonical(result))
                    if size <= 32 * 1024**2:
                        cache[key] = (result, size)
                        cache_bytes += size
                        while len(cache) > 10000 or cache_bytes > 32 * 1024**2:
                            _, (_, removed) = cache.popitem(last=False)
                            cache_bytes -= removed
                output.append(result)
            write_frame(protocol, {"results": output})
        except Exception as error:
            write_frame(protocol, {"error": f"{type(error).__name__}: {error}"})


def summarize(raw, manifest):
    def get(name):
        value = raw.get(name)
        if isinstance(value, dict):
            value = value.get(name)
        return value if value not in (None, "") else None

    def number(name):
        value = get(name)
        return float(value) if value else None

    return {"bbl": get("BOROUGH BLOCK LOT (BBL)"),
            "bin": get("Building Identification Number (BIN) of Input Address or NAP") or get("Building Identification Number (BIN)"),
            "latitude": number("Latitude"), "longitude": number("Longitude"),
            "grc": get("Geosupport Return Code (GRC)"), "grc2": get("Geosupport Return Code 2 (GRC 2)"),
            "message": get("Message"), "message2": get("Message 2"),
            "release": manifest["identity"]["release"], "engine_version": manifest["identity"]["engine_version"],
            "bundle_id": manifest["bundle_id"], "raw": canonical(raw)}


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--engine":
        engine_main(Path(sys.argv[2]))
        return
    manager = Manager()
    def terminate(signum, frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, terminate)
    atexit.register(manager.close)
    try:
        while True:
            line = sys.stdin.buffer.readline(MAX_FRAME + 1)
            if not line:
                break
            try:
                if len(line) > MAX_FRAME or not line.endswith(b"\n"):
                    raise ValueError("Invalid or oversized request")
                result = manager.dispatch(json.loads(line))
                write_frame(sys.stdout.buffer, {"result": result})
            except Exception as error:
                write_frame(sys.stdout.buffer, {"error": f"{type(error).__name__}: {error}"})
    finally:
        manager.close()


if __name__ == "__main__":
    main()
