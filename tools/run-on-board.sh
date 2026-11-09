#!/usr/bin/env bash
# tools/run-on-board.sh <file>
#
# Run the file open in the editor on the board (VS Code's default build task,
# Ctrl+Shift+B): a .py file with the board's `python` (MicroPython, through
# pyboard.py and ssh_python.py), a file in a Rust project under rust/ with
# tools/klauss-rust run.  ssh_shell must be running on the board.
set -uo pipefail
RT=$(cd "$(dirname "$0")/.." && pwd)
f=${1:?usage: tools/run-on-board.sh <file>}
case $(realpath -m "$f") in
  *.py)
    exec python3 "$RT/zephyr-ws/modules/lib/micropython/tools/pyboard.py" \
      -d "exec:python3 $RT/tools/validate/zephyr/ssh_python.py" "$f" ;;
  "$RT"/rust/*)
    exec "$RT/tools/klauss-rust" run "$f" ;;
  *)
    echo "run-on-board: $f is neither a .py file nor in a Rust project under $RT/rust" >&2
    exit 2 ;;
esac
