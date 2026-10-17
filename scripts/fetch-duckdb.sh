#!/usr/bin/env bash
set -euo pipefail
destination=${1:-build/deps}
mkdir -p "$destination"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64)
    cli=linux-amd64; lib=linux-amd64
    cli_sha=ea79eae4233f1aba9a020c8a61877de38a789bc62cdd37485d3589cd77dc0d3e
    lib_sha=09cc288295964d897b47665d1898e16e8ef176cae9ea615797fc136eae15bd5d ;;
  Darwin-arm64)
    cli=osx-arm64; lib=osx-universal
    cli_sha=9daef42173916715d488b7c75f4200bd8c1d93afe2ebbfde4a90d1a30d4ab6fc
    lib_sha=5e6d79f5fb86bbd4f24d59cee3bc38f113d1433761df35337e3c01c62eeafe26 ;;
  Darwin-x86_64)
    cli=osx-amd64; lib=osx-universal
    cli_sha=92efd4f5e11f34b2b694f3bc9ad7c480b09a31193033e2c0c80c0688295dfda0
    lib_sha=5e6d79f5fb86bbd4f24d59cee3bc38f113d1433761df35337e3c01c62eeafe26 ;;
  *) echo 'Supported build hosts: Linux x86-64 and macOS.' >&2; exit 1 ;;
esac
for kind in cli lib; do
  if [[ $kind == cli ]]; then asset="duckdb_cli-$cli.zip"; expected=$cli_sha
  else asset="libduckdb-$lib.zip"; expected=$lib_sha; fi
  archive="$destination/$asset"
  if [[ ! -f $archive ]]; then
    curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
      "https://github.com/duckdb/duckdb/releases/download/v1.4.4/$asset" -o "$archive.part"
    mv "$archive.part" "$archive"
  fi
  if command -v sha256sum >/dev/null; then actual=$(sha256sum "$archive" | cut -d ' ' -f1)
  else actual=$(shasum -a 256 "$archive" | cut -d ' ' -f1); fi
  [[ $actual == "$expected" ]] || { echo "Checksum mismatch: $archive" >&2; exit 1; }
  if [[ $kind == cli ]]; then unzip -oq "$archive" duckdb -d "$destination"
  elif [[ $lib == osx-universal ]]; then unzip -oq "$archive" libduckdb.dylib -d "$destination"
  else unzip -oq "$archive" libduckdb.so -d "$destination"; fi
done
chmod +x "$destination/duckdb"
"$destination/duckdb" --version
