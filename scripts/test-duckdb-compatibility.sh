#!/usr/bin/env bash
# 0.3 targets stable C API v1.5.6; older hosts must reject its metadata cleanly.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-smoke}
case "$mode" in smoke|--e2e) ;; *) echo 'Usage: test-duckdb-compatibility.sh [--e2e]' >&2; exit 1 ;; esac
if [[ $mode == --e2e ]]; then
  [[ -n ${GEOSUPPORT_HOME:-} ]] || { echo '--e2e requires GEOSUPPORT_HOME with installed 26b and 26c bundles' >&2; exit 1; }
fi
make DUCKDB_VERSION=1.5.6 fetch
make -j2 DUCKDB_VERSION=1.5.6 test
extension="$PWD/build/native/1.5.6/geosupport.duckdb_extension"
if command -v sha256sum >/dev/null; then digest() { sha256sum "$extension" | cut -d ' ' -f1; }
else digest() { shasum -a 256 "$extension" | cut -d ' ' -f1; }; fi
baseline_sha=$(digest)
if [[ $mode == --e2e ]]; then make DUCKDB_VERSION=1.5.6 integration; fi
for version in 1.4.4 1.5.0; do
  bash scripts/fetch-duckdb.sh "build/deps/$version" "$version"
  cli="$PWD/build/deps/$version/duckdb"
  if result=$("$cli" -unsigned -bail -no-stdin -c "LOAD '$extension';" 2>&1); then
    echo "Unexpected load on unsupported DuckDB $version" >&2; exit 1
  fi
  [[ $result == *v1.5.6* ]] || { echo "Unexpected failure: $result" >&2; exit 1; }
  [[ $(digest) == "$baseline_sha" ]] || { echo 'Extension changed during compatibility test' >&2; exit 1; }
  echo "PASS: DuckDB $version rejects the v1.5.6 API requirement"
done
