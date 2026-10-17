"""Bounded real-engine benchmark. Reports timings; makes no production capacity claims."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import sys
import time

import duckdb

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--extension", default=str(ROOT / "build/geosupport.duckdb_extension"))
parser.add_argument("--output", default=str(ROOT / "build/benchmark.json"))
args = parser.parse_args()
con = duckdb.connect(config={"allow_unsigned_extensions": True})
con.execute("LOAD '" + args.extension.replace("'", "''") + "'")
start = time.perf_counter()
bundle = con.execute("SELECT bundle_id FROM gs_load('26c')").fetchone()[0]
startup = time.perf_counter() - start
# Distinct requests vary house number on one real street; many will be rejects.
# This stresses calls and serialization, not a representative citywide corpus.
con.execute("CREATE TEMP TABLE requests AS SELECT i::VARCHAR AS house FROM range(1,2001) r(i)")
query = "SELECT sum(length((gs_address(house,'Broadway','Mn',release := '26c')).raw)) FROM requests"
times = []
for _ in range(4):
    start = time.perf_counter()
    checksum = con.execute(query).fetchone()[0]
    times.append(time.perf_counter() - start)
start = time.perf_counter()
con.execute("CREATE TEMP TABLE repeated AS SELECT gs_address('125','Worth St','Mn',release := '26c') AS geo FROM range(10000)")
repeated = time.perf_counter() - start
report = {"duckdb": duckdb.__version__, "host": platform.platform(), "runtime": os.environ.get("GEOSUPPORT_RUNTIME", "local"),
          "bundle_id": bundle, "load_verify_startup_seconds": startup, "rows": 2000,
          "uncached_seconds": times[0], "cached_seconds": times[1:],
          "uncached_rows_per_second": 2000 / times[0],
          "cached_median_rows_per_second": 2000 / statistics.median(times[1:]),
          "repeated_materialization_rows": 10000, "repeated_materialization_seconds": repeated,
          "output_bytes": checksum,
          "limitations": "Serialized native worker calls; 2000 house numbers on Broadway including rejects, not citywide capacity. Docker on Apple Silicon uses amd64 emulation."}
Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
con.close()
