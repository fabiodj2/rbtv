#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

sh "$HERE/test-runtime.sh"
python3 -m unittest discover -s "$HERE" -p 'test_*.py' -v
