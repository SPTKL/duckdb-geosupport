# Native runtime — 0.2.0

NYC's 26C archive ships `include/foruser/geo.h` and a usage guide identifying
`libgeo.so` as a C API. Its declaration is `void geo(char *, char *)`. This is
sufficient for a direct C++ call; it does not establish the implementation
language of every internal Geosupport component.

The DuckDB extension now runs its bundle manager and installer in C++. It starts
an isolated native executable for each loaded bundle. The executable uses
`dlopen`/`dlsym` to resolve `geo`, creates fixed-width work areas, calls the NYC
library directly, and parses the response. No Python interpreter, coordinator,
ctypes calls, or Python UDFs participate in production execution.

## Why retain a worker?

The library and data paths are selected through process environment variables.
Workers give each release its own environment, library dependencies, native state
and failure boundary. Loading multiple quarters into DuckDB's process would need
separate evidence about thread safety, dependency namespaces and reinitialization.
The direct C library call is inside the C++ worker; this is intentionally not an
in-process `dlopen` inside DuckDB. Linux needs no Docker. macOS uses a native Linux
worker container because the NYC binary is Linux x86-64.

## Distribution and compatibility

The build embeds the native executable, normalized layouts and release catalog.
On Linux, first engine use atomically materializes the worker under
`GEOSUPPORT_HOME/native/<sha256>`. The cache must be owned by the current user;
permissions, file type and digest are checked before execution. LOAD, EXPLAIN and
catalog inventory do not materialize a worker. The native worker image has no
Python interpreter and needs only the NYC bundle mounted read-only.

The 0.1 store schema and bundle identities are unchanged. SQL helpers, release
selection, return codes, nested raw fields, flags and supported modes retain their
existing semantics. The pinned upstream Python wrapper remains a test oracle;
the production metadata and parser retain its MIT attribution. Normal native
builds read committed JSON and do not import or execute the wrapper.

## Verification on 2026-10-17

- Linux x86-64 build: all 31 Python-driven regression tests passed, including
  execution with an empty PATH and an invalid `GEOSUPPORT_PYTHON` setting.
- macOS arm64 extension with native Linux Docker workers: 30 passed, one Linux-only
  test skipped. The expanded corpus compares all raw fields for 22 requests on
  each quarter (44 comparisons) against direct calls through the pinned wrapper.
  Independent raw outputs and their provenance are committed under
  `tests/fixtures/expected-*` for native CI regression checks without Python.
- Coverage includes 1B, 1A, 1E, 1N, BL, BN, AP, 2, 2W, 3, 3C and 3S, aliases,
  numeric inputs, NULL fields, auxiliary segments, long/extended/TPAD modes,
  rejected inputs, and both quarterly engines alive together. This remains a
  bounded corpus, not proof of every field/function or citywide correctness.
- Native Make build and C++ offline test harness passed on macOS: 30 store checks
  and 9 SQL checks, without invoking Python for build or tests. The C++ end-to-end
  harness passed 97 checks using the real bundles and native Docker workers,
  including all-field golden comparisons and fixture provenance verification.
- The production worker image was checked for absent Python executables; its
  dependencies are the native C/C++ runtime libraries.
- The native HTTPS downloader installed and unpacked the official 26C archive in
  an empty temporary store. Its resulting bundle ID exactly matched the 0.1
  installation: `bce0275d8f061d45d4cf916647474cda66700544b0fc57c930c30225b5979ecd`.
- A regression exposed a 1 MiB checksum buffer on a small DuckDB query-thread
  stack. Moving that buffer to the heap resolved the macOS crash; the full suite
  then passed.

GitHub's native CI and image workflows are additional validation paths. Their
execution and publication status must be read from Actions; local checks do not
establish that remote CI or registry publication succeeded.

## Remaining limits

Quarterly layout changes still require review and a broader representative corpus.
Real patch-package coverage, cancellation propagation, and DuckDB external-access
policy enforcement remain pending. Workers are bounded to eight and calls are
serialized; no parallel-throughput or production-readiness claim is made. Native
Linux builds executed locally through amd64 emulation on an Apple Silicon host.
Community toolchain portability and signed distribution remain separate work.
