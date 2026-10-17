# Native validation and ready-to-use images

The main CI pipeline is `.github/workflows/images.yml`. It first calls the native
Linux workflow, then builds and tests both quarterly images. Pull requests and
`codex/**` branches validate without publishing. Successful pushes to `main`
publish current release tags; `v*` tags publish versioned tags. Manual runs can
publish from `main` or a version tag by enabling the `publish` input.

## Native Linux job: no Python or Docker

The native job runs directly on an Ubuntu x86-64 runner. It uses C/C++, Make,
shell, and the checksum-pinned DuckDB 1.4.4 CLI/shared library. Neither the build,
test harness, nor benchmark invokes Python or a container runtime. GitHub-hosted
runners may have those programs preinstalled; this is a dependency/execution
claim, not a claim that they are absent from the host OS.

`strace --seccomp-bpf` records executable launches for the complete build/test
script. The audit rejects Python/PyPy or Docker/Podman/containerd/nerdctl
invocations, including absolute paths. The trace is uploaded even after failure.
The benchmark runs under this exec tracing; no cross-host timing thresholds are
used on shared CI infrastructure.

The validation includes:

- Native store tests: checksum failures, cleanup, concurrent/idempotent installs,
  offline imports, identity pinning, tampering, unexpected files, symlinks,
  traversal, duplicate ZIP entries, and special files. These use synthetic ZIPs
  only for installer validation, never as a geocoder.
- DuckDB SQL tests: catalog/inventory, side-effect-free EXPLAIN, missing releases,
  explicit defaults, malformed JSON, and null lifecycle arguments.
- Real 26B and 26C tests: archive import and SQL download/install, full file
  verification, known address/BBL/BIN results, typed/raw consistency, every
  supported output mode, generic function fixtures, per-connection defaults,
  prepared-query release pinning, 5,000 rows across multiple chunks and releases,
  nulls, rejects, malformed fields, recovery, unload/reload, and parallel
  connections. Every raw field for the generic fixture corpus is compared against
  checked-in reference outputs from the pinned historical wrapper on both quarters.
  Their provenance and hashes are recorded in `tests/fixtures/expected-provenance.json`.
- Fresh-process CLI tests: materialize results to a DuckDB database, reopen them
  without loading the extension, and geocode again with unusable network proxies.

This is bounded real-engine coverage. It is not exhaustive coverage of all NYC
addresses, quarterly patches, engine failures, or query cancellation. The legacy
Python oracle tests remain available for separate development comparisons and
reference snapshot generation; they are not executed by this CI pipeline.

Run the same native pipeline on Linux x86-64:

```sh
sudo apt-get install g++ make libcurl4-openssl-dev libssl-dev curl unzip jq strace
mkdir -p build/reports
strace --seccomp-bpf -f -e trace=execve -s 4096 -o build/reports/native-exec.log \
  bash scripts/ci-native.sh
bash scripts/audit-native-execs.sh build/reports/native-exec.log
```

Or use the individual targets:

```sh
make fetch
make -j2 test
export GEOSUPPORT_HOME="$PWD/.cache/geosupport"
export GEOSUPPORT_RUNTIME=local
make integration
make benchmark
make package
```

`make integration` explicitly installs the two catalog releases if missing. The
full CI script uses and removes its own temporary store; it does not alter a
personal Geosupport store. Network access is required for pinned DuckDB assets
and initial NYC bundle installation. The native build and offline tests also
work on macOS; real engine execution is Linux x86-64 only.

## Benchmark output

`build/native/benchmark.json` contains the DuckDB version, host, immutable bundle
ID, startup/verification time, and three cold and warm timings for 2,000 distinct
house-number requests on Broadway. Each cold trial starts a new worker/cache;
each warm trial repeats exactly those requests. The output is consumed and
checked on every run. A separate 10,000-row table measures repeated requests and
result materialization with column inputs to avoid constant folding.

Cold runs measure parsing, engine calls, IPC, and serialization. Warm runs measure
the worker result cache. Repeated rows exercise chunk deduplication. Some Broadway
house numbers are rejects, so this is not representative citywide throughput.
The Actions summary reports median rates; the `native-linux-results` artifact
includes the JSON, logs, executable trace, and extension package. Failures in
correctness or missing/empty benchmark output fail CI; timing variation alone
does not.

## Published images

The image repository is `ghcr.io/sptkl/duckdb-geosupport`:

| Tag | Meaning |
| --- | --- |
| `26b` / `26c` | Most recent successful main build for that quarter |
| `latest` | Most recent successful main build with 26C |
| `vX.Y.Z-26b` / `vX.Y.Z-26c` | Build from that version tag |
| `sha-<full-commit>-26b` / `sha-<full-commit>-26c` | Exact tested source commit |

Images target `linux/amd64`. Each includes DuckDB 1.4.4, the installed extension,
its embedded C++ worker, and one checksum-verified NYC engine/data release. Python,
a compiler, and Docker itself are absent from the final image. Expect a large
image because the real quarterly data is included (roughly 1.9 GiB unpacked).

```sh
docker run --rm --platform linux/amd64 ghcr.io/sptkl/duckdb-geosupport:26c \
  -c "SELECT gs_address('125','Worth St','Mn');"

# Interactive SQL; extension and release default are already configured.
docker run --rm -it --platform linux/amd64 ghcr.io/sptkl/duckdb-geosupport:26c
```

The default entrypoint enables unsigned extensions, selects the installed
extension directory, loads `geosupport`, and sets the quarter default. It runs as
UID 10001. Mount a writable directory at `/data` to persist a DuckDB database;
ensure that UID can write the directory. Geocoding needs a writable worker cache
inside `GEOSUPPORT_HOME`, but it needs no network after the image is built.

CI first tests each image's default entrypoint with `--network none`, including
4,097 rows, BBL lookup, reject codes, the expected quarter, and the non-root user.
Only then does it log in with the workflow's `GITHUB_TOKEN` (`packages: write`)
and push the exact tested image. No registry secret is needed. Repository or
organization package policy must allow publication; GHCR package visibility is
managed separately in GitHub settings.

To build without publishing:

```sh
docker build --platform linux/amd64 --build-arg GEOSUPPORT_RELEASE=26c -t geosupport:26c .
docker run --rm --network none -i geosupport:26c < docker/smoke.sql
```

NYC engine/data terms remain separate from this repository's MIT license. Image
provisioning preserves the upstream bundle payload and its included documentation.
