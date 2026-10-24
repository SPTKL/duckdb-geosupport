# Community preparation validation — 2026-10-24

These are local checks of the 0.3 preparation source tree, not a published release
or a successful GitHub Actions run. Existing user changes were retained; the
approved compatibility change raises the next release's minimum to DuckDB 1.5.6.

## Passed

- Native macOS arm64 build: 30 store checks and 21 SQL checks on DuckDB 1.5.6.
  SQL coverage includes disabled external access, prepared statements, all eight
  lifecycle operations, oversize scalar input, and absence of store side effects.
- CMake/community-layout macOS build: both SQLLogicTest files pass on 1.5.6 and
  on `2.0.1.dev2610211350`, using the same built extension. This development wheel
  is not the exact source snapshot currently selected by community CI. The
  tested macOS artifact SHA-256 is
  `167f439d781b830a0a62be243244485f6324b7f4452666a05b0c34103b7157f4`.
- Compatibility boundary: the new binary loads on 1.5.6; 1.4.4 and 1.5.0 reject
  its v1.5.6 API requirement without disabling metadata validation.
- Actionlint on the new community workflow and modified native workflow;
  shell syntax checks and `git diff --check`.
- Upstream community descriptor parser against a temporary fixture using the
  existing public base SHA. This validates syntax/platform fields only; the
  submission template intentionally still needs the final reviewed source ref.
- The name `geosupport` is absent from the inspected community repository snapshot.

## Linux artifact validation

The community CMake build and both SQLLogicTests passed in the existing Linux
x86-64 development image under emulation. The real engine initially exposed a
missing libm dependency after static C++ runtime linking; CMake now explicitly
retains libm for the worker because NYC's libgeo.so references math symbols
without declaring that dependency itself.

The final community-layout Linux artifact passed **100 real 26B/26C SQL checks**
on DuckDB 1.5.6, including known addresses, BBL/BIN, full-output reference parity,
prepared statements, 5,000-row batches, concurrent connections, worker recovery,
and denying calls after external access is disabled while workers already exist.
The test reused read-only cached quarterly bundles and a separate writable worker
cache; it did not modify the original bundle store.

Artifact SHA-256:
`d711027f9041750866c01c70305a7dc1dd99c012ef4c731bc75cc7d27344f3b4`.
The worker's ELF dependencies are libm, libc, and the dynamic loader; it does not
require a shared libstdc++. The extension's local build still uses system curl,
OpenSSL, and their distribution-specific dependency closure. The build/test log
is in the ignored local file `build/community-linux/validation.log`.

The local image uses system curl/OpenSSL. This does **not** validate the official
manylinux/vcpkg distribution build, TLS certificate discovery in its static curl,
or the supported glibc runtime baseline. The new GitHub workflow must be run on
the final reviewed source ref before publication.

## Remaining limits

Query cancellation during native I/O still waits for completion or the existing
timeout. Neither the inspected v1.5.6 header nor current DuckDB main exposes a
query-interrupted C API check for this adapter.
Do not treat these tests as cancellation validation, citywide throughput evidence,
or approval to publish. See [publication gates](community-publication.md).
