# Third-party components

- **DuckDB C extension headers**: copied from `duckdb/extension-template-c` commit
  `0718cc424a8214e085d1910329b800a0c738e645`. MIT license in
  `vendor/duckdb/LICENSE`. https://github.com/duckdb/duckdb
- **DuckDB metadata tool**: `scripts/append_extension_metadata.py`, from
  `duckdb/extension-ci-tools` commit `3fd6109fc01555673b7c7123eba8d0d143fe8ce4`.
  https://github.com/duckdb/extension-ci-tools
- **nlohmann/json**: v3.11.3, unmodified single header, MIT license in
  `vendor/nlohmann/LICENSE`. https://github.com/nlohmann/json
- **python-geosupport**: commit `82c39e82b4d4dc7773fa67d32ef7f3787ef4d27e`.
  `resources/layouts.json` and the native parser derive field definitions and
  formatting semantics from this MIT-licensed project. License in
  `resources/PYTHON_GEOSUPPORT_LICENSE`. The original Python code is retained
  only under `tests/reference/vendor/geosupport` as a comparison oracle and is
  excluded from runtime packages. The production runtime executes no Python.
  https://github.com/ishiland/python-geosupport
- **miniz**: 3.0.2, ZIP extraction, MIT license in `vendor/miniz/LICENSE`.
  Upstream files are unmodified; `miniz_export.h` supplies the static build export
  macro. https://github.com/richgel999/miniz/tree/3.0.2
- **PicoSHA2**: v1.0.1, small-object SHA-256 and hexadecimal helpers, MIT license
  in `vendor/picosha2/LICENSE`. https://github.com/okdshin/PicoSHA2/tree/v1.0.1
- **libcurl** is dynamically linked for HTTPS downloads. Linux file hashing uses
  dynamically linked OpenSSL; macOS uses system CommonCrypto. These dependencies
  are supplied by the operating system/toolchain, not copied into the extension.
  https://curl.se/docs/copyright.html and https://www.openssl.org/source/license.html

The Geosupport engine and quarterly data are separate NYC distributions. Their
terms are separate from the Python wrapper's MIT license. This repository stores
source URLs and checksums. The extension binary and source packages do not include
those archives. The optional provisioned container images include a separately
downloaded quarterly installation, whose NYC terms still apply.

The provisioned images use Debian bookworm slim pinned by digest in their
Dockerfiles. The standalone Linux release uses Ubuntu 22.04 pinned by digest in
`Dockerfile.release`. `Dockerfile.dev` and the test oracle use a pinned official
Python image for optional development validation only.
