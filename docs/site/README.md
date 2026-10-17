# Documentation site

Public URL: <https://sptkl.github.io/duckdb-geosupport/>.

A single static page for binary download, setup, DuckDB usage, sample output,
functions, and Geosupport return codes and messages. No client-side JavaScript or external assets.
The Colab walkthrough is linked from the page and included as a notebook download.

## Build and preview

From the repository root:

```sh
python3 scripts/build-docs.py
python3 -m http.server 8769 --directory build/site
```

Open <http://localhost:8769>. Edit `docs/site/index.html` and `style.css`.
The build fills the sample-output tables, including the rejected-address example,
from captured results and derives the function-code catalog from
`resources/layouts.json`. The error section covers NYC Geosupport return codes
and messages.
Build from the repository root; publish only `build/site`.

## Colab walkthrough

Edit `notebooks/geosupport_quickstart.ipynb`. The README and site link to its
`main` branch version through Colab; the site build also copies the same notebook
to `build/site/notebooks/` for direct download. Notebook changes trigger Pages.
Keep committed outputs empty and use a Python 3 CPU runtime to run the cells in
order. Optional upload, comparison, and download cells are disabled by default.

The notebook pins DuckDB 1.4.4 and an extension source commit, builds on the Linux
runtime, and downloads the official 26C bundle through `gs_install`. When changing
the source pin or DuckDB version, rerun the notebook from setup through export in
a fresh Linux x86-64 environment. Also exercise the optional quarter comparison;
the CSV upload and download controls require Colab's UI.

## Deployment

`.github/workflows/pages.yml` builds on relevant pushes to `main`, pull requests,
and manual dispatch. Only `main` deploys. Set repository **Settings → Pages →
Source → GitHub Actions** once. Asset paths are relative for the project URL.

The binary download points to a verified successful CI artifact. There is no
versioned release yet. Actions artifacts expire; update the link when a new
binary is published. Setup covers the Linux binary and the published `26c`
container for Macs with Apple Silicon. Source-build instructions live in the
repository README.

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
