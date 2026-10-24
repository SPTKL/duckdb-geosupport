# DuckDB community publication

The 0.3.0 source tree prepares `geosupport` for publication through
[`duckdb/community-extensions`](https://github.com/duckdb/community-extensions).
It is being prepared for **draft review, not publication**. The first distribution target is Linux
x86-64 with glibc; macOS/Docker remains a development path.

After acceptance, the intended user experience is:

```sql
INSTALL geosupport FROM community;
LOAD geosupport;
CALL gs_install('26c');
SET VARIABLE geosupport_release = (SELECT bundle_id FROM gs_load('26c'));
SELECT gs_address('125', 'Worth St', 'Mn');
```

DuckDB installs the adapter only. The explicit `gs_install` downloads the
independently versioned NYC engine/data, whose terms are separate from MIT.

## Implemented in the submission preparation

- Standard C API CMake build with pinned `extension-ci-tools` submodule
  `2de802913caf5c9bba0ef267658b20f3cc0486c3`. `make configure`, `make release`, and
  `make test_release` produce and test
  `build/release/extension/geosupport/geosupport.duckdb_extension`.
- Embedded worker, layouts, and catalog remain self-contained. No Python or
  Docker runtime requirement on Linux. The worker statically links its C++
  runtime; vcpkg supplies static curl/OpenSSL dependencies in community builds.
- Offline SQLLogicTests use a fresh store and perform no NYC downloads. The
  upstream container builds the artifact; tests run in the host's Python
  environment, including on `linux_amd64` where the upstream template normally
  skips them. The separate native workflow retains real 26B/26C validation.
- Stable C API v1.5.6 headers and a DuckDB 1.5.6 minimum. Every runtime request
  checks `enable_external_access`, including prepared queries, reused workers,
  and requests waiting behind another connection. Older hosts must reject the
  metadata; no mismatch bypass is used.
- Per-argument 1 MiB limit and whole-request protocol size check. Existing limits
  include eight live workers, 128 MiB IPC frames, bounded archives/extraction,
  a 300-second worker timeout, checksums, and explicit release pins.
- `.github/workflows/community.yml` invokes DuckDB's distribution toolchain and
  separate offline tests. The descriptor template at
  `community/geosupport/description.yml.in` contains the public repository,
  maintainer, offline example, and explicit platform exclusions. It has no
  invented commit ref and cannot be submitted as-is.

The existing 0.2.0 binary and notebook remain usable on their previously tested
hosts. Version 0.3 deliberately raises the C API floor to enforce access policy.

## Upstream snapshot inspected on 2026-10-24

The [development guide](https://duckdb.org/community_extensions/development)
requires a descriptor PR and a build compatible with the community toolchain.
The C API template is supported; a rewrite against the C++ API is not required
for distribution.

At community repository commit
[`145a7c2f8adcbaf092f405117af498355b2d13aa`](https://github.com/duckdb/community-extensions/tree/145a7c2f8adcbaf092f405117af498355b2d13aa),
`build.yml` targets `v2.0.0` at source commit
`d54cbbe3df17e8e5ce68312f483b4155836112aa` with the `v2.0-cyanoptera` toolchain.
The new workflow includes that snapshot plus v1.5.6. Recheck these pins and name
availability immediately before submission. Prerelease Python wheels are useful
additional evidence, but do not substitute for the exact source snapshot.

## Draft review and remaining publication gates

A draft community PR can be opened now to obtain maintainer feedback and run
upstream CI. The limitations below must be disclosed in that PR. They remain
this project's publication gates, not a claim that DuckDB mandates all of them
before accepting a draft submission.

1. **Prompt cancellation.** The stable C API v1.5.6 exposes client configuration
   but no query-interrupted check usable by this adapter. The current worker and
   curl timeouts bound stalls, but a DuckDB interrupt does not yet promptly stop
   an in-flight native operation. Resolve this with an upstream C API hook or a
   reviewed adapter change, then test cancellation while waiting for a worker,
   during download/verification, and behind a concurrent request. Do not replace
   the execution-time access check with a SQL macro or load-time-only check.
2. **Official distribution CI and clean-host validation.** Run the new workflow
   on the reviewed commit, including the exact community DuckDB snapshot. Local
   system-library builds do not prove the vcpkg/manylinux artifact works. Inspect
   both ELF dependency sets, exercise TLS certificate discovery, and run the real
   quarterly corpus on the supported Linux baseline with only runtime libraries.
   Preserve dependency licenses for the exact static dependency closure.
3. **Reviewed immutable source ref.** Commit and push the reviewed source and
   submodule pin, then replace `@SOURCE_COMMIT@` in the template with that full
   public commit SHA. Run the upstream descriptor parser and workflow again on
   the final ref. Submit it as `extensions/geosupport/description.yml` in a draft
   PR to the community repository. Keep the PR in draft until the release gates
   are resolved or an explicit release decision is documented.

## Validation record

Local preparation checks and their limitations are recorded in
[community validation](community-validation.md). Historical published-binary
compatibility evidence remains in [DuckDB compatibility](duckdb-compatibility.md).

After publication, retain immutable quarterly bundles across adapter upgrades.
Verify archive checksums, layouts, and representative real results before catalog
updates. Track new DuckDB releases and use the community descriptor's supported
version/ref mechanisms when distinct compatibility branches become necessary.
