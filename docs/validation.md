# Validation record — 2026-10-17

## Builds and tests

| Environment | Result |
|---|---|
| macOS arm64 native extension, DuckDB 1.4.4, Python 3.9 coordinator, Linux/amd64 Docker engine workers | 25 tests passed |
| Linux/amd64 native extension, DuckDB 1.4.4, Python 3.12 local workers, Debian trixie container | 25 tests passed |
| Official standalone DuckDB 1.4.4 macOS CLI, unsigned local extension with absolute path | SQL demo passed |
| SQL `gs_install('26c')` into an empty temporary store, followed by `gs_verify` | Download, checksum verification, extraction, publication and file verification passed |

Linux execution occurred through Docker's amd64 emulation on the same Apple
Silicon host. It validates the Linux binary/runtime path, not physical x86 server
performance. CI configuration is supplied but has not run on GitHub.

The 25-test suite includes:

- Catalog inventory, explicit release requirements, malformed JSON and NULL handling.
- No runtime/install side effects from EXPLAIN and no implicit install from geocoding.
- Archive checksum failure, safe extraction, manifest tampering, extra payload files,
  concurrent publication, idempotence, immutable labels, and offline import.
- Actual 26B and 26C workers alive simultaneously, connection-local defaults,
  prepared-query release pinning, and explicit bundle identifiers.
- 5,000 alternating-release rows across multiple chunks, preserving duplicates;
  materialized results remain readable after workers unload.
- Invalid-address GRC/message preservation, typed BIN and BBL lookup, input-width
  validation, and recovery after invalid API requests.
- Full raw-response equality with independent direct wrapper calls for ten requests
  per release (20 comparisons): 1B, 1A, 1E, BL, AP and intersection function 2.

The parity corpus includes ordinary addresses, a Queens hyphenated house number,
an unrecognized street, extended outputs, and a TPAD option. It is bounded and is
not full citywide or production PLUTO parity. Both paths use the pinned upstream
wrapper; this checks the extension's orchestration/serialization against the
existing interface, not the independent correctness of every upstream parser.

## Real source artifacts

| Release | Engine | Original archive SHA-256 |
|---|---|---|
| 26B | 26.2 | `9a0db7df4911f14a4c974de3e19ecac2769e432abd2bbbb183a3593a5d4450be` |
| 26C | 26.3 | `71a3f2d621348d3d70ed2829b425242744d457fc27941f6bf67d807e123bf740` |

Source URLs are recorded in `catalog/releases.json`. These hashes were computed
from the actual official downloads; they are not represented as publisher-signed
checksums. The bundle IDs additionally cover extracted file hashes and metadata:

- 26B: `9adaa8b0ea930c50922cd0665c89a3895d6e99337227e0c64fbea99c6076c966`
- 26C: `bce0275d8f061d45d4cf916647474cda66700544b0fc57c930c30225b5979ecd`

The CLI returned for 125 Worth Street, Manhattan, under 26C:

```text
BBL:       1001680032
BIN:       1001831
Latitude:  40.715669
Longitude: -74.002352
GRC:       00
```

Both actual base releases are installed under this checkout's ignored
`.cache/geosupport` directory, totaling approximately 3.8 GiB. Runtime binaries and
data are excluded from source and distribution packages.

## Bounded performance measurement

The final run used 2,000 distinct house numbers on Broadway, including rejected
addresses, and returned full response JSON. It ran after the test workloads ended.
This workload is not a representative citywide sample. Raw measurements are in
[benchmark-macos.json](benchmark-macos.json).

| Measurement | Result |
|---|---:|
| Verify 26C files and start its worker | 1.785 seconds |
| First pass, 2,000 uncached requests | 1.635 seconds / 1,223 rows per second |
| Median of three cached passes | 0.900 seconds / 2,223 rows per second |
| Materialize 10,000 repeated-address rows | 0.065 seconds |

The cached passes still serialize full responses and do not measure persistent
DuckDB table scan speed. Repeated addresses benefit from deduplication within each
native chunk. These rates are observations on this machine, not service-level
guarantees, scaling estimates, or evidence of a speedup over direct local Geosupport.

## Remaining production work

- Broader real workflow and quarterly layout compatibility coverage.
- Actual UPAD/TPAD patch installation validation and offline patched-bundle import.
- Cancellation propagation, process-failure stress tests and bounded worker scheduling.
- Unique-request profiling and reduction of Python/JSON/full-response overhead.
- Portable Linux binaries, Windows support if required, signing and distribution.
- Review of runtime assets and installation behavior before community submission.

The delivered artifact is a functional development extension with real-engine
validation; it is not a signed or published production release.
