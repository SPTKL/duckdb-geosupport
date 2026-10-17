import json
import os
from pathlib import Path

import duckdb
import pytest

ROOT = Path(__file__).resolve().parents[1]
EXTENSION = Path(os.environ.get("GEOSUPPORT_TEST_EXTENSION", ROOT / "build/geosupport.duckdb_extension"))


def connect():
    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    con.execute("LOAD '" + str(EXTENSION).replace("'", "''") + "'")
    return con


@pytest.fixture
def con(tmp_path, monkeypatch):
    monkeypatch.setenv("GEOSUPPORT_HOME", str(tmp_path / "store"))
    with connect() as connection:
        yield connection


def test_native_extension_inventory(con):
    assert con.sql("SELECT release FROM gs_available_releases() ORDER BY release").fetchall() == [("26b",), ("26c",)]
    assert con.sql("SELECT * FROM gs_installed_releases()").fetchall() == []
    assert con.sql("SELECT * FROM gs_runtime_info()").fetchall() == []


def test_explain_never_installs_or_starts_worker(con):
    con.execute("EXPLAIN SELECT * FROM gs_install('unknown')")
    con.execute("EXPLAIN SELECT gs_address('125','Worth St','Mn',release := 'unknown')")
    assert not Path(os.environ["GEOSUPPORT_HOME"]).exists()


def test_missing_release_errors_without_network(con):
    with pytest.raises(duckdb.Error, match="not installed"):
        con.sql("SELECT gs_address('125','Worth St','Mn',release := '26c')").fetchall()
    assert not Path(os.environ["GEOSUPPORT_HOME"]).exists()


def test_explicit_release_required(con):
    with pytest.raises(duckdb.Error, match="Select a release"):
        con.sql("SELECT gs_address('125','Worth St','Mn')").fetchall()


def test_null_lifecycle_argument(con):
    with pytest.raises(duckdb.Error, match="cannot be NULL"):
        con.sql("CALL gs_load(NULL)").fetchall()


def test_bad_generic_json(con):
    with pytest.raises(duckdb.Error, match="parse_error"):
        con.sql("SELECT gs_call('BL','bad json',release := '26c')").fetchall()


@pytest.fixture(scope="module")
def real():
    home = os.environ.get("GEOSUPPORT_TEST_HOME")
    if not home:
        pytest.skip("Set GEOSUPPORT_TEST_HOME to a store with actual 26B and 26C bundles")
    previous = os.environ.get("GEOSUPPORT_HOME")
    os.environ["GEOSUPPORT_HOME"] = home
    try:
        with connect() as con:
            yield con
    finally:
        if previous is None:
            os.environ.pop("GEOSUPPORT_HOME", None)
        else:
            os.environ["GEOSUPPORT_HOME"] = previous


@pytest.mark.integration
def test_real_address_and_both_releases(real):
    for release in ("26b", "26c"):
        identity = real.execute("SELECT bundle_id FROM gs_load(?)", [release]).fetchone()[0]
        geo = real.execute("SELECT gs_address('125','Worth St','Mn',release := ?)", [identity]).fetchone()[0]
        assert geo["bbl"] == "1001680032"
        assert geo["release"] == release
        assert geo["bundle_id"] == identity
        assert geo["grc"] in ("00", "01")
        assert json.loads(geo["raw"])["BOROUGH BLOCK LOT (BBL)"]["BOROUGH BLOCK LOT (BBL)"] == geo["bbl"]
    assert real.sql("SELECT count(*) FROM gs_runtime_info()").fetchone()[0] == 2


@pytest.mark.integration
def test_connection_defaults_and_prepared_query_pin(real):
    real.execute("SET VARIABLE geosupport_release = '26b'")
    real.execute("PREPARE before_switch AS SELECT (gs_address('125','Worth St','Mn')).release")
    real.execute("SET VARIABLE geosupport_release = '26c'")
    assert real.sql("EXECUTE before_switch").fetchone()[0] == "26b"
    assert real.sql("SELECT (gs_address('125','Worth St','Mn')).release").fetchone()[0] == "26c"
    second = real.cursor()
    with pytest.raises(duckdb.Error, match="Select a release"):
        second.sql("SELECT gs_address('125','Worth St','Mn')").fetchall()
    second.close()


@pytest.mark.integration
def test_chunks_duplicates_nulls_and_materialization(real):
    real.execute("""CREATE OR REPLACE TEMP TABLE geocoded AS
        SELECT i, gs_address('125','Worth St','Mn',release := CASE WHEN i%2=0 THEN '26b' ELSE '26c' END) AS geo
        FROM range(5000) r(i)""")
    assert real.sql("SELECT count(*),count(DISTINCT geo.bundle_id),min(geo.bbl),max(geo.bbl) FROM geocoded").fetchone() == (5000, 2, "1001680032", "1001680032")
    assert real.sql("SELECT gs_call(NULL,'{}',release := '26c')").fetchone()[0] is None
    real.execute("CALL gs_unload('26b')")
    real.execute("CALL gs_unload('26c')")
    assert real.sql("SELECT count(*) FROM gs_runtime_info()").fetchone()[0] == 0
    assert real.sql("SELECT count(*) FROM geocoded").fetchone()[0] == 5000


@pytest.mark.integration
def test_invalid_address_preserves_response(real):
    geo = real.sql("SELECT gs_address('999999','NOT A REAL STREET','Mn',release := '26c')").fetchone()[0]
    assert geo["grc"] not in (None, "00", "01")
    assert geo["message"]
    assert json.loads(geo["raw"])


@pytest.mark.integration
def test_generic_functions_and_input_validation(real):
    assert real.sql("SELECT (gs_bbl('1001680032',release := '26c')).bbl").fetchone()[0] == "1001680032"

    geo = real.sql("SELECT gs_address('125','Worth St','Mn',release := '26c')").fetchone()[0]
    assert geo["bin"]
    assert real.execute("SELECT (gs_bin(?,release := '26c')).bbl", [geo["bin"]]).fetchone()[0] == "1001680032"
    for inputs, error in [('{"unknown": "x"}', "Unknown input"), ('{"house_number": "' + '1'*100 + '"}', "field width")]:
        with pytest.raises(duckdb.Error, match=error):
            real.execute("SELECT gs_call('1B',?,release := '26c')", [inputs]).fetchall()
    # A data error must not poison later requests.
    assert real.sql("SELECT (gs_bbl('1001680032',release := '26c')).bbl").fetchone()[0] == "1001680032"


@pytest.mark.integration
def test_direct_wrapper_parity(real):
    import subprocess
    import sys
    from runtime.manager import DOCKER_IMAGE, Store
    requests = json.loads((ROOT / "tests/fixtures/requests.json").read_text())
    store = Store(os.environ["GEOSUPPORT_TEST_HOME"], ROOT / "catalog/releases.json")
    for release in ("26b", "26c"):
        bundle, _ = store.resolve(release)
        env = os.environ.copy()
        if os.environ.get("GEOSUPPORT_RUNTIME") == "docker":
            command = ["docker", "run", "--rm", "-i", "--pull=never", "--platform", "linux/amd64", "--network=none", "--read-only",
                       "--mount", f"type=bind,src={ROOT},dst=/project,readonly",
                       "--mount", f"type=bind,src={bundle},dst=/bundle,readonly",
                       "-e", "GEOFILES=/bundle/payload/fls/", "-e", "LD_LIBRARY_PATH=/bundle/payload/lib",
                       "-e", "PYTHONDONTWRITEBYTECODE=1", DOCKER_IMAGE, "python", "/project/scripts/direct_reference.py"]
        else:
            env["GEOFILES"] = str(bundle / "payload/fls") + "/"
            env["LD_LIBRARY_PATH"] = str(bundle / "payload/lib")
            command = [sys.executable, str(ROOT / "scripts/direct_reference.py")]
        expected = json.loads(subprocess.run(command, input=json.dumps(requests), text=True, capture_output=True, env=env, check=True, timeout=60).stdout)
        actual = []
        for request in requests:
            result = real.execute("SELECT gs_call(?, ?, release := ?, mode := ?)",
                                  [request["function"], json.dumps(request["params"]), release, request["mode"]]).fetchone()[0]
            actual.append(json.loads(result["raw"]))
        assert actual == expected
