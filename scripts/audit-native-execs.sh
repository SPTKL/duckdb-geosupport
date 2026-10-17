#!/usr/bin/env bash
set -euo pipefail
trace=${1:?Usage: audit-native-execs.sh TRACE}
[[ -s $trace ]] || { echo 'Missing native execution trace' >&2; exit 1; }
# Match executable paths, not arguments that happen to mention these words.
if grep -E 'execve\("([^" ]*/)?(python([0-9.]+)?|pypy([0-9.]+)?|docker|podman|containerd|nerdctl)"' "$trace"; then
  echo 'FAIL: Python or a container runtime was invoked in native CI.' >&2
  exit 1
fi
echo 'PASS: native build, tests, and benchmarks invoked no Python or container runtime.'
