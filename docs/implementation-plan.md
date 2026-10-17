# Geosupport for DuckDB: implementation plan

## Objective

Build a real loadable DuckDB extension that manages quarterly Geosupport bundles
and exposes geocoding in SQL and Python-hosted DuckDB. Preserve the existing
engine's behavior, partial results, and return codes. Do not reimplement its
geographic matching rules.

## Delivery stages

1. Compile a native extension using DuckDB's C extension API. Provide reproducible
   builds and native SQL smoke tests. Python is an internal worker dependency in
   this initial implementation; users do not register Python UDFs.
2. Implement an immutable local bundle store, a checksum-pinned release catalog,
   explicit download/import, safe extraction, inventory, and offline validation.
   Treat engine version, data release, platform and patches as separate metadata.
3. Run persistent workers isolated by bundle, with environment configured before
   the engine starts. Support Linux x86-64 directly and an explicitly selected
   Docker runtime on macOS. Send whole DuckDB chunks through local pipes.
4. Expose generic function calls plus typed address, BBL and BIN helpers. Use
   immutable bundle IDs and explicit SQL connection variables for release defaults;
   never change process-wide environment variables in DuckDB.
5. Validate lifecycle failures, connection isolation, prepared query behavior,
   chunking, nulls, invalid addresses, side-by-side releases, and parity with direct
   python-geosupport. Benchmark real inputs and distinguish native Linux execution
   from x86 emulation on this Apple Silicon machine.
6. Prepare publication through `duckdb/community-extensions`: integrate the
   standard build toolchain, remove the adjacent-runtime-directory requirement,
   harden external access and cancellation, and validate a Linux x86-64 release.
   See [community publication](community-publication.md) for sequencing and gates.

## Initial scope and acceptance

- LOAD a compiled binary from both Python and DuckDB-compatible clients.
- Install, import, list, verify, load and unload supported release bundles via SQL.
- No implicit network requests while geocoding. Existing installations work offline.
- Pin downloads by SHA-256; verify each installed payload file before loading.
- An interrupted install never publishes a partial release. Reinstall is idempotent.
- Return typed common fields and a full raw response, including GRC and messages.
- Geosupport rejects are data; missing/corrupt runtimes and malformed API requests
  are query errors. Never substitute a fake geocoder when the real engine is absent.
- Preserve row count and duplicate inputs. Deduplicate within each chunk and use a
  bounded per-worker memory cache keyed by complete input, function and mode.
- Compare at least two actual quarterly releases with direct calls.

## Design constraints

The initial compiled adapter targets POSIX (Linux/macOS). The upstream engine is
not a macOS library; macOS requires Docker with a Linux/amd64 runtime. Windows and
Wasm builds are deferred. Community publication is the next delivery milestone;
development builds require DuckDB's explicit unsigned-extension setting.

The current native extension owns a C++ manager and one isolated C++ worker per
immutable bundle. The worker calls NYC's C API directly. Layouts and formatting
semantics derive from the MIT-licensed wrapper, which is now only a test oracle.
Calls are serialized; multi-worker scheduling is a follow-up, not an assumed
speedup. The 0.1 Python coordinator described in the original delivery stages
was replaced in 0.2.

The bundle store is outside the DuckDB database. Persist bundle identifiers with
materialized results. A release label is never silently rebound to different bytes.
Patch support uses ordered checksum-pinned overlays on a fresh staging directory,
then publishes a separate bundle identity.

## References

- https://duckdb.org/community_extensions/development
- https://github.com/duckdb/extension-template-c
- https://nycplanning.github.io/Geosupport-UPG/chapters/chapterI/section05/
- https://python-geosupport.readthedocs.io/en/latest/configuration.html
- https://github.com/ishiland/python-geosupport

## Execution status — 2026-10-17

Stages 1–5 are implemented for the bounded 0.1 scope. Compiled macOS arm64 and
Linux amd64 artifacts exist; the 25-test suite passes on both runtime paths. Two
actual official quarterly bundles are installed, the standalone CLI demo works,
and a fresh SQL-driven network installation was verified. See `validation.md` for
evidence and the remaining production work. The public source repository is
[SPTKL/duckdb-geosupport](https://github.com/SPTKL/duckdb-geosupport).
Community extension publication has not been performed.

## Native rewrite — 0.2.0

The Python coordinator and worker have been replaced with C++. The manager now
runs inside the extension and the isolated native worker calls NYC's C API directly.
The executable and layout metadata are embedded, removing the adjacent-directory
and Python runtime requirements. Existing bundle identities remain compatible.
Native Make builds and C++ test harnesses are also available. See
[native-runtime.md](native-runtime.md) for current validation and limits;
the earlier stage descriptions above record the original implementation.

Stage 6 remains partially complete: standalone packaging is implemented, while
community toolchain integration, runtime hardening and submission remain pending.
