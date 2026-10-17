# Third-party components

- **DuckDB C extension headers**: copied from `duckdb/extension-template-c` commit
  `0718cc424a8214e085d1910329b800a0c738e645`. MIT license in
  `vendor/duckdb/LICENSE`. https://github.com/duckdb/duckdb
- **DuckDB metadata tool**: `scripts/append_extension_metadata.py`, from
  `duckdb/extension-ci-tools` commit `3fd6109fc01555673b7c7123eba8d0d143fe8ce4`.
  https://github.com/duckdb/extension-ci-tools
- **nlohmann/json**: v3.11.3, unmodified single header, MIT license in
  `vendor/nlohmann/LICENSE`. https://github.com/nlohmann/json
- **python-geosupport**: commit `82c39e82b4d4dc7773fa67d32ef7f3787ef4d27e`,
  vendored without source changes in `runtime/vendor/geosupport`. MIT license in
  `runtime/vendor/PYTHON_GEOSUPPORT_LICENSE`.
  https://github.com/ishiland/python-geosupport

The Geosupport engine and quarterly data are separate NYC distributions. Their
terms are separate from the Python wrapper's MIT license. This repository stores
source URLs and checksums; it does not redistribute those engine/data archives.

The Docker runtime uses the official Python image pinned by manifest digest in
`runtime/manager.py` and `Dockerfile.dev`.
