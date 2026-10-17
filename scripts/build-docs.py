#!/usr/bin/env python3
"""Build the one-page static guide from committed samples and source metadata."""
import hashlib
import html
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'docs/site'
DEST = ROOT / 'build/site'


def table(rows, columns, css='sample-table'):
    def value(v):
        return '<span class="null">NULL</span>' if v is None else html.escape(str(v))
    return '<div class="table-wrap"><table class="' + css + '"><thead><tr>' + ''.join('<th scope="col">' + html.escape(c) + '</th>' for c in columns) + '</tr></thead><tbody>' + ''.join('<tr>' + ''.join('<td>' + value(row.get(c)) + '</td>' for c in columns) + '</tr>' for row in rows) + '</tbody></table></div>'


def main():
    data = (SOURCE / 'data/samples.json').read_bytes()
    provenance = json.loads((SOURCE / 'data/provenance.json').read_text())
    assert hashlib.sha256(data).hexdigest() == provenance['samples_sha256'], 'Sample provenance mismatch'
    rows = json.loads(data)
    assert len(rows) == provenance['row_count'] == 14
    for row in rows:
        assert row['geo']['bundle_id'] == provenance['bundles'][row['geo']['release']]
    current = {r['sample_id']: r['geo'] for r in rows if r['geo']['release'] == '26c'}
    assert current['worth-st']['bbl'] == current['bbl']['bbl'] == current['bin']['bbl'] == '1001680032'
    columns = ['bbl', 'bin', 'latitude', 'longitude', 'grc', 'grc2']
    layouts = json.loads((ROOT / 'resources/layouts.json').read_text())
    codes = [{'Code': code, 'Aliases': ', '.join(alias for alias, target in layouts['aliases'].items() if target == code) or '—'} for code in layouts['functions']]
    source = (SOURCE / 'index.html').read_text()
    replacements = {
        'FIRST_OUTPUT': table([current['worth-st']], columns),
        'BATCH_OUTPUT': table([dict(id=i, **current[key]) for i, key in enumerate(('worth-st', 'empire-state', 'queens', 'rejected'), 1)], ['id'] + columns),
        'REJECT_OUTPUT': table([dict(id=4, **current['rejected'])], ['id', 'grc', 'grc2', 'message', 'message2']),
        'FUNCTION_CODES': table(codes, ['Code', 'Aliases']),
        'ERROR_OUTPUT': table([{'Field': field, 'Value': current['rejected'][field]}
                               for field in ('grc', 'message', 'grc2', 'message2', 'bbl', 'bin', 'latitude', 'longitude')],
                              ['Field', 'Value'], 'error-output'),
    }
    for key, fragment in replacements.items():
        assert source.count('<!-- ' + key + ' -->') == 1
        source = source.replace('<!-- ' + key + ' -->', fragment)
    assert not re.search(r'<!-- [A-Z_]+ -->', source), 'Unfilled documentation placeholder'
    assert '<script' not in source.lower(), 'The documentation page must stay static'
    DEST.mkdir(parents=True, exist_ok=True)
    # Remove generated assets from the earlier interactive prototype.
    for relative in ('app.js', 'data/samples.sql'):
        (DEST / relative).unlink(missing_ok=True)
    (DEST / 'index.html').write_text(source)
    shutil.copyfile(SOURCE / 'style.css', DEST / 'style.css')
    (DEST / '.nojekyll').touch()
    (DEST / 'data').mkdir(exist_ok=True)
    for name in ('samples.json', 'provenance.json'):
        shutil.copyfile(SOURCE / 'data' / name, DEST / 'data' / name)
    (DEST / 'notebooks').mkdir(exist_ok=True)
    shutil.copyfile(ROOT / 'notebooks/geosupport_quickstart.ipynb', DEST / 'notebooks/geosupport_quickstart.ipynb')
    print(f'Built {DEST}: {len(rows)} sample rows, {len(codes)} function codes')

if __name__ == '__main__':
    main()
