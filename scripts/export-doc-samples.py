#!/usr/bin/env python3
"""Capture documentation examples from the real native extension (no downloads)."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    ('worth-st', '125 Worth St, Manhattan', '1B', {'house_number': '125', 'street_name': 'Worth St', 'borough_code': 'Mn'}, 'regular'),
    ('empire-state', '350 5 Ave, Manhattan', '1B', {'house_number': '350', 'street_name': '5 Ave', 'borough_code': 'Mn'}, 'regular'),
    ('queens', '30-30 Thomson Ave, Queens', '1B', {'house_number': '30-30', 'street_name': 'Thomson Ave', 'borough_code': 'QN'}, 'regular'),
    ('rejected', '999999 NOT A REAL STREET, Manhattan', '1B', {'house_number': '999999', 'street_name': 'NOT A REAL STREET', 'borough_code': 'Mn'}, 'regular'),
    ('bbl', 'BBL 1001680032', 'BL', {'bbl': '1001680032'}, 'regular'),
    ('bin', 'BIN 1001831', 'BN', {'bin': '1001831'}, 'tpad'),
    ('extended', '125 Worth St, extended 1E', '1E', {'house_number': '125', 'street_name': 'Worth St', 'borough_code': 'Mn'}, 'extended'),
]

def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duckdb', type=Path, default=ROOT / 'build/deps/duckdb')
    parser.add_argument('--extension', type=Path, default=ROOT / 'build/native/geosupport.duckdb_extension')
    args = parser.parse_args()
    selects = []
    for release in ('26b', '26c'):
        for ident, label, function, params, mode in CASES:
            selects.append(f"SELECT {quote(ident)} AS sample_id, {quote(label)} AS input_label, {quote(function)} AS function_code, {quote(json.dumps(params))} AS inputs, {quote(mode)} AS mode, to_json(gs_call({quote(function)}, {quote(json.dumps(params))}, release := {quote(release)}, mode := {quote(mode)})) AS geo")
    sql = '\nUNION ALL\n'.join(selects) + ';'
    output = subprocess.check_output([str(args.duckdb.resolve()), '-unsigned', '-bail', '-init', '/dev/null', '-json', '-cmd', 'LOAD ' + quote(args.extension.resolve()) + ';', ':memory:', sql], text=True)
    rows = json.loads(output)
    for row in rows:
        if isinstance(row["geo"], str):
            row["geo"] = json.loads(row["geo"])
    assert len(rows) == 14
    data = (json.dumps(rows, indent=2, ensure_ascii=True) + '\n').encode()
    destination = ROOT / 'docs/site/data'
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'samples.json').write_bytes(data)
    provenance = {
        'method': 'Captured by scripts/export-doc-samples.py using gs_call through the native extension and official NYC engines. No synthetic output.',
        'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'duckdb_version': subprocess.check_output([str(args.duckdb.resolve()), '--version'], text=True).strip(),
        'extension_sha256': hashlib.sha256(args.extension.read_bytes()).hexdigest(),
        'samples_sha256': hashlib.sha256(data).hexdigest(),
        'row_count': len(rows),
        'bundles': {row['geo']['release']: row['geo']['bundle_id'] for row in rows},
    }
    (destination / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(f'Captured {len(rows)} real results in {destination}')

if __name__ == '__main__':
    main()
