#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
g++ -std=c++17 -fPIC "$root/tests/fps_probe.cpp" "$root/native/src/fps_unlock.cpp" \
  -I"$root/native/src" $(pkg-config --cflags --libs Qt6Core sqlite3) -o "$tmp/fps-probe"
python3 "$root/tests/fps_menu_test.py" "$tmp/fps-probe"
python3 "$root/tests/fps_migrate_test.py" "$tmp/fps-probe"
python3 "$root/tests/fps_legacy_decimal_test.py" "$tmp/fps-probe"
