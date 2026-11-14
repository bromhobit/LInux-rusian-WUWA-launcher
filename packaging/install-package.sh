#!/usr/bin/env bash
set -euo pipefail
dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
shopt -s nullglob
pkgs=("$dir"/echo-native-*.pkg.tar.zst)
if (( ${#pkgs[@]} == 0 )); then echo "Не найден пакет ECHO Native в $dir" >&2; exit 2; fi
pkg="$(printf '%s\n' "${pkgs[@]}" | sort -V | tail -n 1)"
if [[ ! -x /usr/bin/pacman ]];then echo "Этот установщик предназначен для Arch Linux / CachyOS." >&2;exit 2;fi
rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/applications/echo-native.desktop" 2>/dev/null || true
echo "Установка: $pkg"
if [[ $EUID -eq 0 ]]; then
  pacman -U -- "$pkg"
elif command -v pkexec >/dev/null 2>&1 && [[ ! -t 0 ]]; then
  pkexec /usr/bin/pacman -U --noconfirm -- "$pkg"
elif command -v pkexec >/dev/null 2>&1; then
  pkexec /usr/bin/pacman -U -- "$pkg"
else
  sudo pacman -U -- "$pkg"
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
  kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
  kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi
