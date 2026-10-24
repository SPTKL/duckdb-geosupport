# DuckDB host compatibility

The upcoming **0.3.0** adapter requires **DuckDB 1.5.6+**, targeting stable
`C_STRUCT v1.5.6`. Client-context configuration access is needed to honor
`enable_external_access` at execution time. The native build defaults to the
checksum-pinned 1.5.6 host; 1.4.4 and 1.5.0 must reject the new binary cleanly.
Run `bash scripts/test-duckdb-compatibility.sh` to check that boundary.

The community pipeline additionally targets the active 2.0 prerelease snapshot.
That workflow must pass before publication. A stable C API target is a
compatibility mechanism, not evidence that every later host has been tested.

The notebook still downloads published **0.2.0**, and the evidence below describes
that version and its earlier source tree. Its old build commands and compatibility
matrix are historical; they do not describe 0.3 source builds.

## Historical 0.2 compatibility evidence

The supported, checksum-pinned host versions are **1.4.4, 1.5.0, and 1.5.6**.
New builds, development dependencies, and the Colab notebook default to 1.5.6.
These are tested versions, not a promise that every future DuckDB release works.

The extension's metadata targets the versioned C extension ABI `C_STRUCT v1.2.0`.
The host DuckDB version and extension ABI version are different: building with
1.4.4 does not restrict this extension to a 1.4.4 host. We retain the existing
vendored headers and ABI target and test the same binary across host versions.
No `allow_extensions_metadata_mismatch` bypass is needed. The development binary
is unsigned, so `allow_unsigned_extensions` (or CLI `-unsigned`) is still required.

## Build and select a host

```sh
make DUCKDB_VERSION=1.5.6 fetch
make DUCKDB_VERSION=1.5.6 -j2 test
build/deps/1.5.6/duckdb -unsigned -bail -init /dev/null \
  -cmd "LOAD '$PWD/build/native/1.5.6/geosupport.duckdb_extension';"
```

Use the same `DUCKDB_VERSION` for every Make invocation. Downloads and build
outputs are separated by version, so selecting another version cannot silently
reuse a prior host's shared library. The SQL harness prints the actual library
version and fails if it differs from the selected version. The fetch script
rejects unsupported pins and checks every archive against
`scripts/duckdb-checksums.txt`. Existing unversioned build directories are not used
by the new defaults.

Docker builds accept `--build-arg DUCKDB_VERSION=1.5.0` (or another supported pin).
This applies to `Dockerfile` and `Dockerfile.release`; the separate Linux worker
image does not contain DuckDB. Existing published images are unchanged until a
new image is built and published.

## Regression coverage

The native Linux CI matrix runs unit tests, real 26B/26C integration tests, and
benchmarks for each pinned host. The 1.4.4 job also runs
`scripts/test-duckdb-compatibility.sh --e2e` against its unchanged extension:

- Each CLI loads the same file and queries the catalog.
- Each version's shared-library harness runs unit and full real-engine tests.
- Each CLI performs a known-address lookup.
- The extension's SHA-256 must remain unchanged throughout the matrix.

The full suite checks typed/raw results, generic fixtures, release selection,
prepared statements, multiple connections, 5,000-row batches, nulls, rejects,
and worker recovery. It reuses the installed bundles within the baseline job.
For a quick offline load test, run `bash scripts/test-duckdb-compatibility.sh`.
For full coverage, set `GEOSUPPORT_HOME` to a store with 26B and 26C and pass
`--e2e`. On macOS, also set `GEOSUPPORT_RUNTIME=docker` and build the worker image.

## Published v0.2.0 evidence

The existing release archive was checked against its published SHA-256:

```text
Archive:   e2157e513f8e47b6d9fc1a097f222f6520f14b171cf8a353a726e12eb54752f2
Extension: 43b52a2bdee3d3e4449b4ba05e8c58ee020e41f31b8d4bb499e64d75515602b7
```

On 2026-10-17, the exact published Linux extension passed offline tests with
DuckDB 1.4.4, 1.5.0, and 1.5.6 using the existing 26C image's real data. Checks
covered 4,097 address rows, BBL lookup, release inventory, and rejected-address
return codes, with container networking disabled. The archive and its original
build provenance have not been replaced.

The updated notebook also passed from setup through CSV/Parquet/ZIP export in
fresh Ubuntu 22.04 and 24.04 containers using DuckDB 1.5.6 and that same release
archive. Both runs checked four real sample rows, bundle provenance, cached setup,
and repeat exports without installing a compiler. Colab-specific upload/download
UI controls were not exercised. A native macOS arm64 build on 1.5.6 passed 30
store checks, 10 SQL unit checks, and all 98 real 26B/26C integration checks using
the Linux worker image. The same 1.4.4-built macOS extension then passed 10 SQL
unit checks and 98 integration checks under each of 1.4.4, 1.5.0, and 1.5.6.
Its SHA-256 remained
`01cb9a52baf5bfd364c0baeadadbaf4ea4bd038ff6f8af3fdb49c63f3322a5dc`.

A source-built Linux x86-64 baseline also passed 30 store checks, followed by
10 SQL unit checks and 98 real 26B/26C integration checks on each pinned host.
Each harness printed and asserted its actual DuckDB shared-library version. The
same extension was used throughout, with SHA-256
`078731ade2f5e89e7e6977b5f6ba471778705505c0dc30c1684d16b850809975`.
These local Linux checks ran in a Debian container under x86-64 emulation; the
configured GitHub Actions matrix has not been run as part of this local change.

Fetch validation also covered unsupported versions, a corrupted cached archive,
and concurrent fetches while running SQL tests. The fetcher stages and atomically
replaces binaries, avoiding truncation of a shared library already in use.

This removes the 1.4.4-only validation limitation for a Linux Moses geocoding
stage. It does not remove NYC's Linux x86-64 engine requirement: native macOS
hosts still need the Linux worker. Windows, Wasm, and Linux arm64 remain outside
this extension's supported runtime targets. Tests cover bounded real fixtures,
not citywide throughput or a complete Moses pipeline run.
