# DuckDB community publication

Target: publish `geosupport` through
[`duckdb/community-extensions`](https://github.com/duckdb/community-extensions).
This is the distribution target for the next milestone. The current 0.1 build
is a development prototype and is not ready for a submission.

## Intended user experience

After acceptance and publication, on a supported platform:

```sql
INSTALL geosupport FROM community;
LOAD geosupport;

-- Explicitly download an independently versioned NYC engine/data bundle.
CALL gs_install('26c');
SET VARIABLE geosupport_release =
    (SELECT bundle_id FROM gs_load('26c'));

SELECT gs_address('125', 'Worth St', 'Mn');
```

These community INSTALL/LOAD commands are a target, not commands that work today.
The same SQL should work in Python's DuckDB connection without registering UDFs.
DuckDB installs the adapter; `gs_install` installs the quarterly engine/data.
Keep extension version, DuckDB compatibility, engine version, and bundle identity
independent. Historical bundles must remain usable after an adapter upgrade,
subject to explicit layout compatibility checks.

## Verified upstream requirements and precedents

- Publication uses a descriptor PR pointing to the extension's source repository
  and version. The extension must build with DuckDB's CI toolchain. See the
  [development guide](https://duckdb.org/community_extensions/development).
- The community repository already includes
  [capi_quack](https://github.com/duckdb/community-extensions/blob/2f4bebb893ab6512ab553c320385c1dae2ac00e4/extensions/capi_quack/description.yml).
  Retain our C extension API foundation; publication does not require rewriting
  the adapter to DuckDB's internal C++ API.
- The inspected CI uploads `.duckdb_extension` artifacts, and the community
  deployment signs and distributes them. Our tarball and adjacent `runtime/`
  directory are not installed by that path. See the
  [artifact upload workflow](https://github.com/duckdb/extension-ci-tools/blob/3fd6109fc01555673b7c7123eba8d0d143fe8ce4/.github/workflows/_extension_distribution.yml)
  and [community deployment](https://github.com/duckdb/community-extensions/blob/2f4bebb893ab6512ab553c320385c1dae2ac00e4/.github/workflows/_extension_deploy.yml).
- Descriptors support platform exclusions and toolchain requirements. Python and
  process-related extensions already exist; this is not evidence that our design
  will be accepted, but there is no basis here for a blanket claim that Python or
  subprocesses are forbidden. See
  [python_udf](https://github.com/duckdb/community-extensions/blob/2f4bebb893ab6512ab553c320385c1dae2ac00e4/extensions/python_udf/description.yml),
  [shellfs](https://github.com/duckdb/community-extensions/blob/2f4bebb893ab6512ab553c320385c1dae2ac00e4/extensions/shellfs/description.yml),
  and the [descriptor parser](https://github.com/duckdb/community-extensions/blob/2f4bebb893ab6512ab553c320385c1dae2ac00e4/scripts/build.py).

Sources inspected on 2026-10-17. Recheck the active build matrix and submission
rules before publishing; a pinned example is evidence, not a permanent policy.

## Implementation sequence and release gates

The following gates are our engineering requirements, not a claim that DuckDB
mandates every design choice.

1. **Make the build compatible with community CI.** Add the C API template's
   CMake/Makefile integration and pin `extension-ci-tools`. Produce the expected
   `build/release/extension/geosupport/geosupport.duckdb_extension` artifact and
   provide SQLLogicTests runnable through the standard test target. Keep Python
   tests for the bundle store. Our existing script and Debian development image
   do not establish portable community-build compatibility.
2. **Remove the adjacent-directory installation requirement.** The proposed
   first-release path embeds the coordinator, vendored wrapper, notices, and
   catalog as resources in the extension. Materialize them atomically into a
   private, content-addressed runtime cache on explicit runtime use, validating
   ownership, permissions, symlinks and checksums before execution. Never depend
   on the checkout or download mutable worker code. Python remains a documented
   system prerequisite for this path; a native worker is a later option, not a
   community requirement. Prove installation using only the resulting binary in
   a clean environment with the documented prerequisites. No writes, downloads
   or worker startup during LOAD or EXPLAIN.
3. **Ship Linux x86-64 first.** Target the community `linux_amd64` toolchain and
   test the actual NYC shared libraries on the documented runtime baseline.
   Compiler portability alone does not establish Geosupport engine compatibility.
   Explicitly exclude unsupported platform entries from the current matrix.
   Keep macOS plus Docker as a development path until its distribution and runtime
   prerequisites have independent release validation. Do not advertise Linux
   ARM, musl, Windows or Wasm support from the existing tests.
4. **Harden runtime access and resource use.** Honor DuckDB's external-access
   configuration before filesystem/network/process operations, including an
   already-started coordinator. Confirm the required APIs and version floor; if
   the current stable C API cannot enforce this, resolve that before release.
   Propagate cancellation, bound request sizes and live workers, and verify
   shutdown, worker crashes and concurrent queries. Preserve explicit downloads
   and immutable release pins. A signed extension does not sandbox NYC binaries
   that it subsequently installs and executes.
5. **Validate the publication artifact.** Run offline SQL smoke tests with no
   Geosupport download in ordinary community CI. Separately run real-engine
   integration tests on the packaged binary against two quarterly releases,
   including parity, rejects, prepared statements and upgrade behavior. Test the
   DuckDB versions targeted by the active community workflow; 1.4.4 results alone
   are insufficient. Record native Linux benchmarks before making throughput
   claims. Keep multi-gigabyte data out of the source, extension and CI artifacts.
6. **Prepare and submit the descriptor.** Use the public source repository
   `SPTKL/duckdb-geosupport` and maintainer `SPTKL`, with a reviewed immutable
   commit for `repo.ref`,
   and add `extensions/geosupport/description.yml` in the community repository.
   Include version, description, MIT adapter license, platform exclusions,
   toolchains and a minimal SQL example. Document the separate NYC engine/data
   terms and required Python runtime. Build-time `requires_toolchains` does not
   install Python for end users. Recheck name availability, run upstream CI, and
   submit only after the above gates pass. No descriptor PR has been opened.

## Maintenance after publication

Maintain adapter compatibility as DuckDB releases change. Use the community
descriptor's `ref_next` mechanism when separate upcoming-release compatibility
work is needed. For each quarterly NYC release, verify official archive checksums,
layout changes and representative results before adding a catalog entry. Ship
catalog updates through reviewed extension releases initially; keep the existing
explicit local catalog override for controlled deployments. Do not add automatic
remote catalog refresh or silently select a new default quarter.

Current blockers are build integration, standalone runtime packaging, runtime
hardening and release-matrix validation. The source repository is public under
SPTKL; the community descriptor comes after those implementation steps.
