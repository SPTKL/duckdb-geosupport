# Geosupport for DuckDB

**[Documentation](https://sptkl.github.io/duckdb-geosupport/)** — binary download,
setup, SQL examples, sample output, and the function/error reference.
See [site maintenance](docs/site/README.md) to build or publish it.

**[Open the walkthrough in Google Colab](https://colab.research.google.com/github/SPTKL/duckdb-geosupport/blob/main/notebooks/geosupport_quickstart.ipynb)**
— build and load the extension, install 26C, geocode a table, inspect Geosupport
return codes, and export CSV/Parquet results with provenance. Use a Python 3 CPU
runtime; initial setup takes several minutes and several GiB of disk. Optional
cells accept your own CSV and compare quarters. No Docker or GitHub token is needed.
[View or download the notebook](notebooks/geosupport_quickstart.ipynb).

A native DuckDB extension for NYC Geosupport, with quarterly release management
and typed SQL results. **Version 0.2.0 removes Python from the runtime.**

```text
DuckDB → C++ extension → isolated C++ worker → NYC libgeo.so + quarterly data
```

NYC provides a C API: `void geo(char *wa1, char *wa2)`. The worker loads that
library directly. The installer, checksum verification, input formatting, output
parsing, batching and cache are C++. A worker per bundle isolates the library and
its `GEOFILES`/`LD_LIBRARY_PATH` settings; different quarters can run simultaneously.

The extension embeds its native worker, layouts and catalog. Copying the
`.duckdb_extension` binary is sufficient; no adjacent runtime directory or pip
package is needed. Linux x86-64 runs directly. macOS needs the native Linux worker
image because NYC's Linux library cannot execute on macOS. Windows/Wasm are not
implemented. This is an **unsigned development release**, not yet available through
`INSTALL geosupport FROM community`. See the [publication plan](docs/community-publication.md).

## Build

The native build uses C++17, Make, curl, unzip, and DuckDB 1.4.4. On Debian/Ubuntu,
install `build-essential libcurl4-openssl-dev libssl-dev curl unzip`. macOS uses the
Command Line Tools and system libcurl/CommonCrypto.

```sh
git clone https://github.com/SPTKL/duckdb-geosupport.git
cd duckdb-geosupport
make fetch             # Checksum-pinned DuckDB CLI and C library
make -j2 test          # Native build and offline C++ tests; no Python required
export GEOSUPPORT_HOME="$PWD/.cache/geosupport"
```

On macOS, build the native Linux worker image and select it explicitly:

```sh
docker build --platform linux/amd64 -f Dockerfile.runtime \
  -t duckdb-geosupport-runtime:0.2.0 .
export GEOSUPPORT_RUNTIME=docker
```

The worker image and its build use no Python. Workers run without networking
and never pull images implicitly. On Linux x86-64, leave `GEOSUPPORT_RUNTIME` unset
or set it to `local`; Docker is unnecessary.

```sh
build/deps/duckdb -unsigned -bail -init /dev/null \
  -cmd "LOAD '$PWD/build/native/geosupport.duckdb_extension';"
```

Use an absolute LOAD path with the hardened macOS CLI. Linux binaries built on a
recent distribution are not automatically portable to older glibc systems; the
community toolchain remains a release milestone.

## SQL and Python-hosted DuckDB

```sql
-- Explicit download: approximately 195 MiB compressed / 1.9 GiB installed.
CALL gs_install('26c');

-- Warm and pin this connection to an immutable engine/data bundle.
SET VARIABLE geosupport_release =
    (SELECT bundle_id FROM gs_load('26c'));

SELECT gs_address('125', 'Worth St', 'Mn');
SELECT gs_bbl('1001680032', release := '26c');
SELECT gs_bin('1001831', release := '26c', mode := 'tpad');

CALL gs_install('26b');
SELECT gs_address('125','Worth St','Mn', release := '26b') AS previous,
       gs_address('125','Worth St','Mn', release := '26c') AS current;
```

The same SQL works from Python's DuckDB package; Python is the optional host here,
not part of the extension's implementation:

```python
import duckdb
from pathlib import Path

con = duckdb.connect('planning.duckdb', config={'allow_unsigned_extensions': True})
binary = Path('build/native/geosupport.duckdb_extension').resolve()
con.execute("LOAD '" + str(binary).replace("'", "''") + "'")
con.execute("CALL gs_install('26c')")
print(con.sql("SELECT gs_address('125','Worth St','Mn',release := '26c')").fetchone()[0])
```

Generic calls accept the established python-geosupport field names and aliases:

```sql
SELECT gs_call('1E',
    to_json({'house_number':'125', 'street_name':'Worth St', 'borough_code':'Mn'}),
    release := '26c', mode := 'extended');

CREATE TABLE geocoded AS
SELECT id, gs_address(house_number, street_name, borough, release := '26c') AS geo
FROM addresses;
SELECT id, geo.bbl, geo.bin, geo.latitude, geo.longitude, geo.grc FROM geocoded;
```

Results contain `bbl`, `bin`, `latitude`, `longitude`, `grc`, `grc2`, `message`,
`message2`, `release`, `engine_version`, `bundle_id`, and full `raw` JSON text.
Geosupport rejects and partial results stay rows with return codes; inspect both
GRCs for function 1B. Malformed API inputs, unsupported functions, damaged bundles,
and worker failures raise query errors. Inputs must fit field widths and be ASCII
without NUL. NULL entire generic function/inputs/mode produces NULL; individual
NULL fields become blanks. `gs_address` invokes 1B and does not implement a separate
PLUTO 1A/1E fallback policy.

## Releases and lifecycle

```sql
SELECT * FROM gs_available_releases();
SELECT * FROM gs_installed_releases();
SELECT * FROM gs_runtime_info();
CALL gs_import('26c', '/absolute/path/linux_geo26c_26.3.zip');
CALL gs_verify('26c');
CALL gs_load('26c');
CALL gs_unload('26c');
```

Lifecycle functions return `release`, `bundle_id`, `engine_version`, `platform`,
and `details` JSON text. Loading does not select a default: use the connection
variable above. Prepared statements capture that variable when prepared. Explicit
release arguments override it. Version 0.2 reads existing 0.1 bundle stores without
migration; bundle identities are unchanged.

The curated catalog includes 26B/26.2 and 26C/26.3. Archive hashes were computed
from official NYC downloads, not publisher-signed checksums. Installation verifies
SHA-256, rejects unsafe ZIP entries, stages privately, and publishes atomically.
Repeating installation verifies and reuses the bundle. No geocoding call installs
missing data. Offline import supports base bundles. Ordered patch overlays are
implemented but have no real-patch validation or catalog entries yet.

The store is separate from the DuckDB file:

```text
GEOSUPPORT_HOME/
  aliases/26c.json
  bundles/<bundle-id>/manifest.json
  bundles/<bundle-id>/payload/{lib,fls,...}
  native/<worker-sha256>     # Verified embedded worker, Linux only
```

Retain bundle IDs and the store for reproducible historical results. Labels never
silently switch to different bytes. Inventory does not hash payloads; new worker
startup does. Up to eight workers are retained, with idle workers evicted as needed.
Calls are serialized; this version does not scale geocoding with DuckDB's threads.
Identical inputs within each chunk are deduplicated, and each worker has a bounded
10,000-entry / 32 MiB result cache.

| Variable | Meaning |
|---|---|
| `GEOSUPPORT_HOME` | Store; default `~/.cache/duckdb-geosupport` |
| `GEOSUPPORT_CATALOG` | Optional explicit local catalog file |
| `GEOSUPPORT_RUNTIME` | `local` (Linux x86-64) or `docker` |
| `GEOSUPPORT_DOCKER` | Docker executable; default `docker` |
| `GEOSUPPORT_DOCKER_IMAGE` | Native worker image; default `duckdb-geosupport-runtime:0.2.0` |

Store/catalog settings are captured on first use. Runtime settings apply when a
worker starts. `GEOSUPPORT_PYTHON` is obsolete and ignored.

Use trusted analytical sessions. The extension writes files and executes installed
native code; lifecycle changes are outside SQL transactions. Query cancellation
and DuckDB external-access policy integration remain production-hardening work.
Worker requests have a 300-second deadline. Community signing does not sandbox
separately downloaded NYC libraries.

## Validation and development

```sh
make test
make integration       # Real bundles; GEOSUPPORT_HOME must be set
make benchmark
make package
```

An optional Python test suite compares complete C++ output with a pinned historical
wrapper. That wrapper lives only under `tests/reference`, is excluded from runtime
packages, and supplies no production execution path. The checked-in layout metadata
is derived from its MIT-licensed field definitions; attribution remains necessary.

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python scripts/build.py                 # Alternate developer build
GEOSUPPORT_TEST_HOME="$GEOSUPPORT_HOME" .venv/bin/python -m pytest -q tests
```

See [native CI, benchmarks and provisioned images](docs/ci.md),
[native design and validation](docs/native-runtime.md), the historical
[0.1 validation](docs/validation.md), and [third-party notices](THIRD_PARTY_NOTICES.md).
NYC engine/data archives are downloaded separately and are not included in source
or extension packages.
