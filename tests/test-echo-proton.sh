#!/usr/bin/env bash
set -Eeuo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
launcher="$project_dir/scripts/echo-proton.sh"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT

export ECHO_DATA_DIR="$tmp/data with spaces"
export STEAM_ROOT="$tmp/Steam Client"
mkdir -p -- "$STEAM_ROOT"

"$launcher" install --dry-run >"$tmp/install-dry-run.txt"
[[ ! -e "$ECHO_DATA_DIR" ]] || { echo "dry-run created data directory" >&2; exit 1; }

"$launcher" install >"$tmp/install.txt"
cmp -- "$project_dir/input/ECHO.exe" "$ECHO_DATA_DIR/app/ECHO.exe"

"$launcher" launch --proton "$tmp/missing-proton" >"$tmp/invalid-proton.txt" 2>&1 && {
  echo "invalid Proton path unexpectedly succeeded" >&2; exit 1;
}

cat >"$tmp/fake-proton" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
{
  printf 'STEAM_COMPAT_DATA_PATH=%s\n' "${STEAM_COMPAT_DATA_PATH:-}"
  printf 'STEAM_COMPAT_CLIENT_INSTALL_PATH=%s\n' "${STEAM_COMPAT_CLIENT_INSTALL_PATH:-}"
  printf 'STEAM_COMPAT_APP_ID=%s\n' "${STEAM_COMPAT_APP_ID:-}"
  printf 'SteamAppId=%s\n' "${SteamAppId:-}"
  printf 'SteamGameId=%s\n' "${SteamGameId:-}"
  printf 'ARG=%s\n' "$@"
} >"$PROTON_TEST_LOG"
EOF
chmod +x "$tmp/fake-proton"
export PROTON_TEST_LOG="$tmp/proton-invocation.txt"

"$launcher" launch --proton "$tmp/fake-proton" >"$tmp/launch-dry-run.txt"
[[ ! -e "$ECHO_DATA_DIR/compatdata" ]] || { echo "launch dry-run created prefix" >&2; exit 1; }
grep -Fq 'No prefix created; ECHO.exe was not run.' "$tmp/launch-dry-run.txt"

"$launcher" launch --execute --proton "$tmp/fake-proton" >"$tmp/launch-execute.txt"
grep -Fq "STEAM_COMPAT_DATA_PATH=$ECHO_DATA_DIR/compatdata" "$PROTON_TEST_LOG"
grep -Fq "STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM_ROOT" "$PROTON_TEST_LOG"
grep -Fxq 'ARG=run' "$PROTON_TEST_LOG"
grep -Fxq "ARG=$ECHO_DATA_DIR/app/ECHO.exe" "$PROTON_TEST_LOG"
[[ -d "$ECHO_DATA_DIR/compatdata" ]]

echo "PASS: hash-verified staging, no-op dry runs, isolated Proton environment, and argument forwarding"
echo "PASS: fake Proton did not execute ECHO.exe"
