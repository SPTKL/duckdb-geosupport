"""Build the C-API extension without compiling DuckDB itself."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

import duckdb

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", default=str(ROOT / "build"))
args = parser.parse_args()
out = Path(args.output).resolve()
out.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX", "c++")
library = out / "geosupport.so"
command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-fPIC", "-fvisibility=hidden",
           "-DDUCKDB_EXTENSION_NAME=geosupport", "-I" + str(ROOT / "vendor/duckdb"),
           "-I" + str(ROOT / "vendor"), str(ROOT / "src/geosupport.cpp"), "-o", str(library)]
command += ["-dynamiclib"] if platform.system() == "Darwin" else ["-shared", "-ldl", "-pthread"]
subprocess.run(command, check=True)
target = duckdb.sql("PRAGMA platform").fetchone()[0]
subprocess.run([sys.executable, str(ROOT / "scripts/append_extension_metadata.py"), "-l", str(library),
                "-n", "geosupport", "-o", str(out / "geosupport.duckdb_extension"), "-p", target,
                "-dv", "v1.2.0", "-ev", "0.1.0", "--abi-type", "C_STRUCT"], check=True)
shutil.copytree(ROOT / "runtime", out / "runtime", dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
shutil.copyfile(ROOT / "catalog/releases.json", out / "runtime/releases.json")
print("Built:", out / "geosupport.duckdb_extension")
