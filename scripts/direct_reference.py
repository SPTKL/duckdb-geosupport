"""Independent direct-wrapper oracle: run INSIDE an initialized Linux engine environment."""
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime/vendor"))
from geosupport import Geosupport, GeosupportError

protocol = os.fdopen(os.dup(1), "w")
os.dup2(2, 1)
g = Geosupport()
results = []
for request in json.load(sys.stdin):
    params = request["params"].copy()
    mode = request["mode"]
    if mode == "extended+tpad":
        mode = "extended"
        params["tpad"] = True
    try:
        result = g[request["function"]](params, mode=mode)
    except GeosupportError as error:
        if not error.result:
            raise
        result = error.result
    results.append(result)
json.dump(results, protocol)
protocol.flush()
