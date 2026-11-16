#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
cmake -S "$root/native" -B "$tmp/build" -DECHO_BUILD_FPS_TESTS=ON
cmake --build "$tmp/build" --target echo-fps-probe -j2
python3 "$root/tests/fps_menu_test.py" "$tmp/build/echo-fps-probe"
python3 "$root/tests/fps_migrate_test.py" "$tmp/build/echo-fps-probe"
python3 "$root/tests/fps_legacy_decimal_test.py" "$tmp/build/echo-fps-probe"
