"""Build native components. Python is a build/test tool, never a runtime dependency."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--output', default=str(ROOT / 'build'))
parser.add_argument('--worker-only', action='store_true')
args = parser.parse_args()
out = Path(args.output).resolve()
out.mkdir(parents=True, exist_ok=True)
generated = out / 'generated'
generated.mkdir(exist_ok=True)

def resource(name, symbol, source):
    content = json.dumps(json.loads(source.read_text()), separators=(',', ':'), ensure_ascii=True)
    (generated / name).write_text('#pragma once\ninline constexpr char ' + symbol + '[] = ' + json.dumps(content) + ';\n')

resource('resources.hpp', 'LAYOUTS_JSON', ROOT / 'resources/layouts.json')
resource('catalog.hpp', 'CATALOG_JSON', ROOT / 'catalog/releases.json')
compiler = os.environ.get('CXX', 'c++')
flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-fPIC', '-fvisibility=hidden',
         '-DDUCKDB_EXTENSION_NAME=geosupport', '-I' + str(ROOT / 'vendor/duckdb'), '-I' + str(ROOT / 'vendor'), '-I' + str(generated)]

def obj(name, suffix='', extra=()):
    path = out / (name + suffix + '.o')
    subprocess.run([compiler, *flags, *extra, '-c', str(ROOT / 'src' / (name + '.cpp')), '-o', str(path)], check=True)
    return str(path)

worker_common = obj('common', '-worker', ['-DGS_WORKER_ONLY'])
engine = obj('engine')
worker_main = obj('worker_main')
platform_libs = [] if platform.system() == 'Darwin' else ['-ldl', '-pthread']
worker = out / 'geosupport-worker'
subprocess.run([compiler, worker_common, engine, worker_main, '-o', str(worker), *platform_libs], check=True)
if args.worker_only:
    print('Built native engine worker:', worker)
    sys.exit(0)

worker_bytes = worker.read_bytes()
common = obj('common')
crypto_libs = [] if platform.system() == 'Darwin' else ['-lcrypto']
(generated / 'worker.hpp').write_text('#pragma once\ninline constexpr unsigned char WORKER_BYTES[] = {\n' +
    ',\n'.join(','.join(str(b) for b in worker_bytes[i:i+32]) for i in range(0, len(worker_bytes), 32)) +
    '\n};\ninline constexpr size_t WORKER_SIZE = sizeof(WORKER_BYTES);\n' +
    'inline constexpr char WORKER_SHA256[] = "' + hashlib.sha256(worker_bytes).hexdigest() + '";\n')
zip_objects = []
for name in ['miniz', 'miniz_zip', 'miniz_tinfl']:
    path = out / (name + '.o')
    subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-fPIC', '-fvisibility=hidden',
                   '-DMINIZ_NO_DEFLATE_APIS', '-DMINIZ_NO_ZLIB_APIS',
                   '-c', str(ROOT / 'vendor/miniz' / (name + '.c')), '-o', str(path)], check=True)
    zip_objects.append(str(path))
store = obj('store')
manager = obj('manager')
adapter = obj('geosupport')
store_main = obj('store_main')
subprocess.run([compiler, common, store, store_main, *zip_objects, '-lcurl', *crypto_libs, '-o', str(out / 'geosupport-store')], check=True)
library = out / 'geosupport.so'
subprocess.run([compiler, adapter, common, manager, store, *zip_objects, '-lcurl',
                '-dynamiclib' if platform.system() == 'Darwin' else '-shared', *platform_libs, *crypto_libs, '-o', str(library)], check=True)
import duckdb
subprocess.run([sys.executable, str(ROOT / 'scripts/append_extension_metadata.py'), '-l', str(library),
    '-n', 'geosupport', '-o', str(out / 'geosupport.duckdb_extension'), '-p', duckdb.sql('PRAGMA platform').fetchone()[0],
    '-dv', 'v1.2.0', '-ev', '0.2.0', '--abi-type', 'C_STRUCT'], check=True)
print('Built Python-free extension:', out / 'geosupport.duckdb_extension')
