#!/bin/sh
# Run from any working directory; game paths may contain spaces.
set -eu
setup_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for setup_python in python3.12 python3; do
    if command -v "$setup_python" >/dev/null 2>&1 && \
       "$setup_python" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
        exec "$setup_python" "$setup_root/tools/setup_halo.py" "$@"
    fi
done
printf '%s\n' 'Python 3.12 is needed. Install it from python.org, or run: brew install python@3.12' >&2
exit 2
