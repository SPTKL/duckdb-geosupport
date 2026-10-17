# Geosupport for DuckDB

A loadable DuckDB extension for NYC Geosupport, with explicit quarterly release
installation, isolated runtimes, and typed SQL results.

The current implementation is a working **0.1 development build**. The SQL adapter
is compiled C++; it communicates in batches with a private Python coordinator and
the vendored `python-geosupport` wrapper. It does not register Python UDFs. Both
Python-hosted DuckDB and the DuckDB CLI can load the binary.

The intended distribution target is **DuckDB Community Extensions**. See the
[publication plan](docs/community-publication.md) for the remaining build,
standalone runtime packaging, and Linux x86-64 release requirements.

## Quick start in this checkout

Requirements: Python 3.9+, a C++17 compiler, and DuckDB 1.4.4 for the validated
build. Linux x86-64 runs the engine locally. macOS runs the engine in Docker;
the extension itself runs natively on macOS. Windows and Wasm are not implemented.

```sh
git clone https://github.com/SPTKL/duckdb-geosupport.git
cd duckdb-geosupport
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python scripts/build.py
export GEOSUPPORT_HOME="$PWD/.cache/geosupport"
```

On macOS, also run:

```sh
export GEOSUPPORT_RUNTIME=docker
docker pull --platform linux/amd64 \
  python:3.12-slim@sha256:a6e34c598f2467ed0e9a8d349809fcd8b5c603269512df273a0bb1784edc11b1
```

The worker never pulls an image implicitly. Docker must be running. On Linux
x86-64, leave `GEOSUPPORT_RUNTIME` unset or set it to `local`.

```python
import duckdb

con = duckdb.connect("planning.duckdb", config={"allow_unsigned_extensions": True})
con.execute("LOAD 'build/geosupport.duckdb_extension'")

# Downloads are explicit. Approximately 195 MiB compressed / 1.9 GiB installed
# per quarterly release. Repeating installation verifies and reuses the bundle.
con.execute("CALL gs_install('26c')")

# Validate files, initialize the worker, and pin the connection to exact bytes.
con.execute("""
    SET VARIABLE geosupport_release =
        (SELECT bundle_id FROM gs_load('26c'))
""")

print(con.sql("SELECT gs_address('125', 'Worth St', 'Mn')").fetchone()[0])
```

The catalog currently contains the official **26B / 26.2** and **26C / 26.3** base
releases, with checksums computed from the downloaded official archives. These
are not claims that newer releases or patches do not exist. Catalog refresh is a
reviewed maintenance operation; it never changes an installed alias silently.

For the DuckDB CLI, start with `duckdb -unsigned` and execute the same SQL, using
an **absolute** extension path (required by hardened macOS CLI builds):

```sh
duckdb -unsigned -bail \
  -cmd "LOAD '$PWD/build/geosupport.duckdb_extension';" < examples/demo.sql
```

Keep
the `runtime/` directory adjacent to the extension binary. This project is not
published to the DuckDB community repository; `INSTALL geosupport FROM community`
is not available. Use the local `LOAD` path above.

## SQL interface

```sql
SELECT * FROM gs_available_releases();
SELECT * FROM gs_installed_releases();

CALL gs_install('26b');
CALL gs_import('26c', '/absolute/path/linux_geo26c_26.3.zip');
CALL gs_verify('26c');
CALL gs_load('26c');
SELECT * FROM gs_runtime_info();
CALL gs_unload('26b');
```

Lifecycle functions return `release`, `bundle_id`, `engine_version`, `platform`,
and `details` (JSON text). `gs_load` warms a runtime; it **does not change a global
or connection default**. Use `SET VARIABLE` as above to choose the default. Release
labels and immutable 64-character bundle IDs are accepted for loaded operations.
The variable is per connection. Prepared queries capture its value at preparation.

`gs_import` verifies a local original archive against a catalog entry; it does not
register an arbitrary unverified directory. Offline import currently supports
base releases. The installer supports ordered, checksum-pinned patch ZIP overlays
into `fls/`; the bundled catalog has no patch entries, and a real patch has not
yet been validated. Never overwrite the bytes associated with a release label:
give patched combinations their own labels.

The scalar helpers return a STRUCT:

```sql
CREATE TABLE geocoded AS
SELECT
    id,
    gs_address(house_number, street_name, borough, release := '26c') AS geo
FROM addresses;

SELECT id, geo.bbl, geo.bin, geo.latitude, geo.longitude, geo.grc
FROM geocoded;

SELECT gs_bbl('1001680032', release := '26c');
SELECT gs_bin('1001831', release := '26c', mode := 'tpad');

-- Full function interface, with python-geosupport input names.
SELECT gs_call(
    '1E',
    to_json({'house_number': '125', 'street_name': 'Worth St', 'borough_code': 'Mn'}),
    release := '26c',
    mode := 'extended'
);

-- Compare quarters within one query.
SELECT
    gs_address('125', 'Worth St', 'Mn', release := '26b') AS prior,
    gs_address('125', 'Worth St', 'Mn', release := '26c') AS current;
```

`gs_address` uses function **1B**, matching the upstream `g.address` alias. It does
not merge the separate PLUTO 1A/1E fallback workflow. Use `gs_call` to implement
that policy explicitly. All upstream functions and field aliases are available
through `gs_call`; bounded parity tests cover 1B, 1A, 1E, BL, AP, and 2, with BN
tested separately. This is not a claim of exhaustive coverage of every function.

Common fields are `bbl`, `bin` (VARCHAR), `latitude`, `longitude` (DOUBLE), `grc`,
`grc2`, `message`, `message2`, `release`, `engine_version`, `bundle_id`, and `raw`.
`raw` is JSON text containing every field parsed by the pinned upstream wrapper.
Missing common fields become SQL NULL; raw empty strings and return codes are
preserved. Coordinates have the semantics of the requested Geosupport function.

Geosupport rejects and partial matches remain rows with return codes. The caller
must inspect both GRCs, especially for 1B. Invalid API fields, unsupported function
names, corrupt installations, and runtime failures raise query errors. Input
fields must fit their fixed widths and contain ASCII without NUL; they are never
silently truncated by this adapter. A NULL entire generic input/function/mode
returns a NULL result; NULL individual address fields are sent as blank fields.

## Runtime and reproducibility

```text
DuckDB native extension
  -> chunk deduplication and local JSON-lines IPC
  -> private coordinator
      -> one persistent isolated worker per immutable bundle
          -> python-geosupport -> Geosupport shared libraries + fls files
```

Engine environment variables are set before each worker starts. DuckDB's own
process environment is never switched between releases. Workers process calls
sequentially; coordinator access is serialized. This version does not scale
geocoding with DuckDB's thread count. Cached results are bounded by 10,000 entries
and 32 MiB per worker, and are not persisted. Identical requests within a DuckDB
chunk cross the IPC boundary only once, then expand back into their original rows.

The store is independent of the database:

```text
GEOSUPPORT_HOME/
  aliases/26c.json
  bundles/<sha256-of-manifest-identity>/
    manifest.json
    payload/lib/
    payload/fls/
```

Archives and each payload file have SHA-256 checksums. Extraction rejects path
traversal and symlinks, installation stages in a temporary directory, and alias
publication is atomic. `gs_load` verifies payloads before starting a new worker.
Inventory lists metadata without hashing; `details.verified` is false for that
operation. Preserve bundle IDs with materialized outputs and retain the store
for historical reruns. Moving a `.duckdb` file alone does not move its runtimes.

Load only in trusted analytical sessions: this extension intentionally launches
local processes and installs native code/data. Lifecycle filesystem changes are
outside DuckDB transactions and are not rolled back by SQL ROLLBACK. The binary
uses an unsigned development build, not a signed distribution. SQL network access
is limited to explicit `gs_install`, but this is not a sandbox for untrusted SQL.

Configuration is read when the coordinator first starts:

| Variable | Meaning |
|---|---|
| `GEOSUPPORT_HOME` | Store location; default `~/.cache/duckdb-geosupport` |
| `GEOSUPPORT_CATALOG` | Optional path to a maintained catalog JSON file |
| `GEOSUPPORT_RUNTIME` | `local` (Linux x86-64) or `docker` |
| `GEOSUPPORT_PYTHON` | Python executable for the coordinator; default `python3` |
| `GEOSUPPORT_DOCKER` | Docker executable; default `docker` |

Runtime requests have a 300-second deadline. A query cancellation is not yet
propagated into an in-flight worker call; this needs hardening before production.

## Build, test, and package

```sh
make test

export GEOSUPPORT_TEST_HOME="$PWD/.cache/geosupport"
make integration

.venv/bin/python scripts/benchmark.py
.venv/bin/python scripts/package.py
```

Integration tests require both real releases installed. Unit tests use explicitly
synthetic archives only to test installation failures; synthetic data is never
used as a geocoding engine. See [validation](docs/validation.md) for measured results
and [implementation plan](docs/implementation-plan.md) for scope.

Linux builds can be produced on macOS using the supplied image:

```sh
docker build --platform linux/amd64 -f Dockerfile.dev -t geosupport-dev:0.1.0 .
docker run --rm --platform linux/amd64 --network none \
  --mount "type=bind,src=$PWD,dst=/workspace" \
  -e GEOSUPPORT_TEST_HOME=/workspace/.cache/geosupport \
  -e GEOSUPPORT_TEST_EXTENSION=/workspace/build/linux/geosupport.duckdb_extension \
  geosupport-dev:0.1.0 sh -c \
  'python scripts/build.py --output build/linux && python -m pytest -q tests'
```

The native adapter uses the DuckDB C extension API with a v1.2.0 API target. The
validated host version is DuckDB 1.4.4; broader compatibility is not assumed. The
Linux binary is built on Debian trixie, so it is not a manylinux portability build.

## Follow-up work

- Profile unique-request parsing and full-response serialization; evaluate a native
  worker and projected outputs before claiming high-throughput production capacity.
- Add query cancellation, bounded worker scheduling, and concurrent-query stress tests.
- Validate quarterly layout changes and real patch packages against representative
  PLUTO/Facilities inputs, including condo lots, TPAD and ambiguous matches.
- Complete the [community publication milestone](docs/community-publication.md):
  standard CI builds, a binary containing its worker assets, runtime hardening,
  and portable Linux x86-64 validation before submitting a descriptor PR.

See [third-party notices](THIRD_PARTY_NOTICES.md). Geosupport binaries/data are
downloaded separately from NYC and are not included in the source or package.
