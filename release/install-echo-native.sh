#!/usr/bin/env bash
set -euo pipefail
dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
shopt -s nullglob
pkgs=("$dir"/echo-native-*.pkg.tar.zst)
if (( ${#pkgs[@]} != 1 )); then echo "Ожидался один пакет ECHO Native в $dir" >&2; exit 2; fi
if [[ ! -x /usr/bin/pacman ]];then echo "Этот установщик предназначен для Arch Linux / CachyOS." >&2;exit 2;fi
echo "Установка: ${pkgs[0]}"
if [[ $EUID -eq 0 ]]; then exec pacman -U -- "${pkgs[0]}"; fi
if command -v pkexec >/dev/null 2>&1;then exec pkexec /usr/bin/pacman -U -- "${pkgs[0]}"; fi
exec sudo pacman -U -- "${pkgs[0]}"
