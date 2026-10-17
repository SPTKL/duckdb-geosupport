"""Regenerate checked-in layout metadata from the pinned TEST oracle, not at runtime."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/reference/vendor'))
from geosupport.function_info import FUNCTIONS, WORK_AREA_LAYOUTS
from geosupport.config import BOROUGHS

spec = {
    'source_commit': '82c39e82b4d4dc7773fa67d32ef7f3787ef4d27e',
    'functions': {k: {m: v[m] for m in ['regular', 'extended', 'long', 'long+tpad']} for k, v in FUNCTIONS.items()},
    'aliases': FUNCTIONS.alt_names, 'layouts': WORK_AREA_LAYOUTS, 'boroughs': BOROUGHS,
}
content = json.dumps(spec, sort_keys=True, indent=2) + '\n'
target = ROOT / 'resources/layouts.json'
if '--check' in sys.argv:
    if target.read_text() != content:
        raise SystemExit('Checked-in layouts differ from the pinned reference')
else:
    target.write_text(content)
