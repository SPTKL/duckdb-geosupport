#!/usr/bin/env bash
# This entire script is traced by CI; no Python or container runtime may execute.
set -euo pipefail
cd "$(dirname "$0")/.."
DUCKDB_VERSION=${DUCKDB_VERSION:-1.5.6}
export DUCKDB_VERSION
export GEOSUPPORT_RUNTIME=local
unset GEOSUPPORT_CATALOG
make fetch
make -j2 test
export GEOSUPPORT_HOME
GEOSUPPORT_HOME=$(mktemp -d "${TMPDIR:-/tmp}/gs-native-ci.XXXXXX")
trap 'rm -rf "$GEOSUPPORT_HOME"' EXIT
extension="$PWD/build/native/$DUCKDB_VERSION/geosupport.duckdb_extension"
duckdb="$PWD/build/deps/$DUCKDB_VERSION/duckdb"
reports="build/reports/$DUCKDB_VERSION"
mkdir -p "$reports"
# Exercise real offline import separately from the SQL network installer (26C).
curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
  "$(jq -r '.releases["26b"].archive.url' catalog/releases.json)" -o build/26b.zip
"$duckdb" -unsigned -bail -no-stdin -c "LOAD '$extension'; CALL gs_import('26b', '$PWD/build/26b.zip');"
rm build/26b.zip
make integration
make benchmark
bash scripts/test-duckdb-compatibility.sh
# A fresh CLI process persists results; another reopens the database offline.
# Proxies are deliberately unusable after explicit installation.
export HTTPS_PROXY=http://127.0.0.1:1 ALL_PROXY=http://127.0.0.1:1
"$duckdb" -unsigned -bail -no-stdin "$GEOSUPPORT_HOME/results.duckdb" -c "
LOAD '$extension';
SET VARIABLE geosupport_release=(SELECT bundle_id FROM gs_load('26c'));
CREATE TABLE results AS SELECT gs_address('125','Worth St','Mn') AS geo;
SELECT CASE WHEN geo.bbl='1001680032' THEN true ELSE error('CLI geocode mismatch') END FROM results;"
"$duckdb" -bail -no-stdin "$GEOSUPPORT_HOME/results.duckdb" -c "
SELECT CASE WHEN count(*)=1 AND bool_and(geo.bbl='1001680032' AND length(geo.bundle_id)=64)
THEN true ELSE error('Persisted result mismatch') END FROM results;"
PATH='' GEOSUPPORT_PYTHON=/does/not/exist "$duckdb" -unsigned -bail -no-stdin -c "
LOAD '$extension';
SELECT CASE WHEN (gs_address('125','Worth St','Mn',release := '26c')).bbl='1001680032'
THEN true ELSE error('Offline geocode failed') END;"
cp "build/native/$DUCKDB_VERSION/benchmark.json" "$reports/benchmark.json"
make package
