# Documentation site

Public URL: <https://sptkl.github.io/duckdb-geosupport/>.

A single static page for binary download, setup, DuckDB usage, sample output,
functions, and errors. No client-side JavaScript or external assets.

## Build and preview

From the repository root:

```sh
python3 scripts/build-docs.py
python3 -m http.server 8769 --directory build/site
```

Open <http://localhost:8769>. Edit `docs/site/index.html` and `style.css`.
The build fills the sample-output tables from captured results and derives the
function-code and diagnostic catalogs from `resources/layouts.json` and `src/*.cpp`.
Build from the repository root; publish only `build/site`.

## Deployment

`.github/workflows/pages.yml` builds on relevant pushes to `main`, pull requests,
and manual dispatch. Only `main` deploys. Set repository **Settings → Pages →
Source → GitHub Actions** once. Asset paths are relative for the project URL.

The binary download points to a verified successful CI artifact. There is no
versioned release yet. Actions artifacts expire; update the link when a new
binary is published. The page includes source-build instructions as a fallback.

## Refresh samples

Build the extension and install 26B and 26C first. Then:

```sh
export GEOSUPPORT_HOME="$PWD/.cache/geosupport"
# macOS only; Docker must be running and the worker image built:
export GEOSUPPORT_RUNTIME=docker
python3 scripts/export-doc-samples.py
python3 scripts/build-docs.py
```

The capture script does not install or download engine data. It records the
extension hash, source commit, DuckDB version, sample hash, and bundle IDs.
The build verifies the hash and identities. Captured adapter results are distinct
from the independent golden fixtures used by native regression tests.
