#!/usr/bin/env bash
set -Eeuo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
out="$root/release"
stage="$root/build/arch-package"
mkdir -p "$out" "$stage"
cp "$root/packaging/PKGBUILD" "$stage/PKGBUILD"
pkgver="$(sed -n 's/^pkgver=//p' "$stage/PKGBUILD")"
tar -czf "$stage/echo-native-source-$pkgver.tar.gz" -C "$root" native packaging/echo-native.desktop packaging/echo-native.svg packaging/LICENSE-PACKAGING packaging/normalize-elf-isa.py
checksum="$(sha256sum "$stage/echo-native-source-$pkgver.tar.gz" | awk '{print $1}')"
sed -i "s/^sha256sums=.*/sha256sums=('$checksum')/" "$stage/PKGBUILD"
(cd "$stage" && makepkg --force --clean --noconfirm)
pkgver="$(sed -n 's/^pkgver=//p' "$stage/PKGBUILD")"
pkgrel="$(sed -n 's/^pkgrel=//p' "$stage/PKGBUILD")"
package_file="$stage/echo-native-${pkgver}-${pkgrel}-x86_64.pkg.tar.zst"
[[ -f "$package_file" ]] || { echo "Expected package not found: $package_file" >&2; exit 1; }
rm -f "$out"/echo-native-*.pkg.tar.zst
cp "$package_file" "$out/"
cp "$stage/PKGBUILD" "$out/PKGBUILD"
cp "$stage/echo-native-source-$pkgver.tar.gz" "$out/"
cp "$root/packaging/install-package.sh" "$out/install-echo-native.sh"
chmod +x "$out/install-echo-native.sh"
(cd "$out" && sha256sum echo-native-*.pkg.tar.zst) | tee "$out/SHA256SUMS"
echo "Ready: $out"

