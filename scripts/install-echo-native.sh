#!/usr/bin/env bash
set -Eeuo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(cd -- "$script_dir/.." && pwd -P)"
prefix="${1:-${XDG_DATA_HOME:-$HOME/.local/share}/echo-native}"
binary="$project_dir/build/native/echo-native"
[[ -x "$binary" ]] || { printf 'Build first: cmake -S "%s/native" -B "%s/build/native" && cmake --build "%s/build/native"\n' "$project_dir" "$project_dir" "$project_dir" >&2; exit 1; }
mkdir -p -- "$prefix/bin" "${XDG_DATA_HOME:-$HOME/.local/share}/applications" "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
install -m 0755 -- "$binary" "$prefix/bin/echo-native"
install -m 0644 -- "$project_dir/packaging/echo-native.svg" "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps/echo-native.svg"
if [[ -f /usr/share/applications/echo-native.desktop && "$prefix/bin/echo-native" != "/usr/bin/echo-native" ]]; then
  rm -f -- "${XDG_DATA_HOME:-$HOME/.local/share}/applications/echo-native.desktop"
  desktop_note="/usr/share/applications/echo-native.desktop (system package)"
else
  cat >"${XDG_DATA_HOME:-$HOME/.local/share}/applications/echo-native.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ECHO Native
GenericName=Wuthering Waves Translation Launcher
Comment=Установка и управление русской локализацией Wuthering Waves
TryExec=$prefix/bin/echo-native
Exec="$prefix/bin/echo-native"
Icon=echo-native
Terminal=false
Categories=Game;Utility;
Keywords=Wuthering Waves;WuWa;ECHO;Русификатор;Перевод;
StartupWMClass=echo-native
StartupNotify=true
EOF
  desktop_note="${XDG_DATA_HOME:-$HOME/.local/share}/applications/echo-native.desktop"
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
  kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
  kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi
printf 'Installed user-local app: %s\nDesktop entry: %s\n' "$prefix/bin/echo-native" "$desktop_note"
