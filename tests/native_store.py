"""Test adapter for the C++ store harness; contains no installation implementation."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('GEOSUPPORT_TEST_EXTENSION', ROOT / 'build/geosupport.duckdb_extension')).parent
ORACLE_DOCKER_IMAGE = 'python:3.12-slim@sha256:a6e34c598f2467ed0e9a8d349809fcd8b5c603269512df273a0bb1784edc11b1'

def sha256(path):
    with open(path, 'rb') as f:
        digest = hashlib.sha256()
        for block in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def invoke(request, root=None, catalog=None):
    env = os.environ.copy()
    if root is not None:
        env['GEOSUPPORT_HOME'] = str(root)
    if catalog is not None:
        env['GEOSUPPORT_CATALOG'] = str(catalog)
    proc = subprocess.run([str(BUILD / 'geosupport-store')], input=json.dumps(request),
                          text=True, capture_output=True, env=env, timeout=120)
    response = json.loads(proc.stdout)
    if 'error' in response:
        raise ValueError(response['error'])
    if proc.returncode:
        raise RuntimeError(proc.stderr)
    return response['result']

class Store:
    def __init__(self, root, catalog):
        self.root, self.catalog_path = Path(root), Path(catalog)
    def _call(self, op, **kwargs):
        return invoke(dict(op=op, **kwargs), self.root, self.catalog_path)
    def install(self, name, archive=None):
        return self._call('install', name=name, archive=str(archive) if archive else '')
    def resolve(self, name):
        out = self._call('resolve', name=name)
        return Path(out['path']), out['manifest']
    def verify(self, name):
        return self._call('verify', name=name)
    def installed(self):
        return self._call('installed')

def safe_extract(archive, destination):
    return invoke(dict(op='extract', archive=str(archive), destination=str(destination)))
