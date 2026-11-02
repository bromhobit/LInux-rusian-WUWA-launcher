#!/usr/bin/env bash
set -Eeuo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
out="$root/release"
stage="$root/build/arch-package"
mkdir -p "$out" "$stage"
cp "$root/packaging/PKGBUILD" "$stage/PKGBUILD"
tar -czf "$stage/echo-native-source-0.2.1.tar.gz" -C "$root" native packaging/echo-native.desktop packaging/echo-native.svg packaging/LICENSE-PACKAGING
checksum="$(sha256sum "$stage/echo-native-source-0.2.1.tar.gz" | awk '{print $1}')"
sed -i "s/^sha256sums=.*/sha256sums=('$checksum')/" "$stage/PKGBUILD"
(cd "$stage" && makepkg --force --clean --noconfirm)
cp "$stage"/echo-native-*.pkg.tar.zst "$out/"
cp "$stage/PKGBUILD" "$out/PKGBUILD"
cp "$root/packaging/install-package.sh" "$out/install-echo-native.sh"
chmod +x "$out/install-echo-native.sh"
sha256sum "$out"/echo-native-*.pkg.tar.zst | tee "$out/SHA256SUMS"
echo "Ready: $out"
