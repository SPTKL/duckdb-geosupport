"""Package a local unsigned build, its worker assets, documentation and licenses."""
import argparse
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build", default=str(ROOT / "build"))
parser.add_argument("--output", default=str(ROOT / "build/geosupport-0.1.0-local.tar.gz"))
args = parser.parse_args()
build = Path(args.build)
with tarfile.open(args.output, "w:gz") as archive:
    for name in ("geosupport.duckdb_extension", "runtime"):
        archive.add(build / name, arcname=name, filter=lambda i: None if "__pycache__" in i.name or i.name.endswith(".pyc") else i)
    for name in ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "docs", "vendor/duckdb/LICENSE", "vendor/nlohmann/LICENSE"):
        archive.add(ROOT / name, arcname=name)
print(args.output)
