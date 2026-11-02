#!/usr/bin/env bash
set -Eeuo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(cd -- "$script_dir/.." && pwd -P)"
binary="${ECHO_NATIVE_BIN:-$project_dir/build/native/echo-native}"
if [[ ! -x "$binary" ]]; then
  printf 'ECHO Native is not built. Run: cmake -S "%s/native" -B "%s/build/native" && cmake --build "%s/build/native"\n' "$project_dir" "$project_dir" "$project_dir" >&2
  exit 1
fi
exec "$binary" "$@"
