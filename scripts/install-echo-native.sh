#!/usr/bin/env bash
set -Eeuo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(cd -- "$script_dir/.." && pwd -P)"
prefix="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/echo-native}"
binary="$project_dir/build/native/echo-native"
[[ -x "$binary" ]] || { printf 'Build first: cmake -S "%s/native" -B "%s/build/native" && cmake --build "%s/build/native"\n' "$project_dir" "$project_dir" "$project_dir" >&2; exit 1; }
mkdir -p -- "$prefix/bin" "${XDG_DATA_HOME:-$HOME/.local/share}/applications"
install -m 0755 -- "$binary" "$prefix/bin/echo-native"
cat >"${XDG_DATA_HOME:-$HOME/.local/share}/applications/echo-native.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ECHO Native
Comment=Русификатор Wuthering Waves — текстовый перевод
Exec="$prefix/bin/echo-native"
Icon=applications-games
Terminal=false
Categories=Game;Utility;
EOF
printf 'Installed user-local app: %s\nDesktop entry: %s\n' "$prefix/bin/echo-native" "${XDG_DATA_HOME:-$HOME/.local/share}/applications/echo-native.desktop"
