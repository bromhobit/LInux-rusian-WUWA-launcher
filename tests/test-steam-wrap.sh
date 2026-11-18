#!/usr/bin/env bash
set -Eeuo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
export ECHO_NATIVE_TEST_BINARY="${ECHO_NATIVE_TEST_BINARY:-$project_dir/build/native/echo-native}"
python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import tempfile

binary = Path(os.environ['ECHO_NATIVE_TEST_BINARY']).resolve()
assert binary.is_file() and os.access(binary, os.X_OK), 'Build build/native/echo-native first'

with tempfile.TemporaryDirectory(prefix='echo-steam-wrap-') as temporary:
    root = Path(temporary)
    steam = root / 'Steam root with spaces'
    apps = steam / 'steamapps'
    game = apps / 'common' / 'Wuthering Waves'
    client = game / 'Wuthering Waves Game'
    mount = client / 'Client/Saved/Resources/3.8.0/Mount'
    shipping = client / 'Client/Binaries/Win64/Client-Win64-Shipping.exe'
    mount.mkdir(parents=True)
    shipping.parent.mkdir(parents=True)
    (apps / 'appmanifest_3513350.acf').write_text('"AppState" { "appid" "3513350" "installdir" "Wuthering Waves" }\n')
    (game / 'launcher.exe').write_text('fake launcher: never execute')
    (client / 'Wuthering Waves.exe').write_text('fake client: never execute')
    shipping.write_text('fake shipping exe: never execute')
    (mount / 'MountResource.txt').write_text('::Mount::\n::Del::\n')
    config = client / 'launcherDownloadConfig.json'
    config.write_text('{"bundles":{"HD":{"version":"3.8.0"}}}')

    log = root / 'invocation.json'
    fake = root / 'fake proton'
    fake.write_text('''#!/usr/bin/python3
import json, os, sys
with open(os.environ['ECHO_FAKE_LOG'], 'w') as f:
    json.dump({'arguments': [os.fsencode(x).hex() for x in sys.argv[1:]],
               'appid': os.environ.get('SteamAppId'),
               'compat': os.environ.get('STEAM_COMPAT_DATA_PATH')}, f)
sys.exit(37)
''')
    fake.chmod(0o755)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('STEAM_', 'Steam'))}
    env.pop('ECHO_OFFICIAL_LAUNCHER', None)
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
               STEAM_ROOT=str(steam), SteamAppId='3513350',
               STEAM_COMPAT_DATA_PATH=str(root / 'compat'), ECHO_FAKE_LOG=str(log),
               QT_QPA_PLATFORM='echo-invalid-qt-platform', SDL_VIDEODRIVER='echo-invalid-video',
               VK_DRIVER_FILES=str(root / 'missing-vulkan.json'))
    launcher = os.fsencode(game / 'launcher.exe')
    shipping_arg = os.fsencode(shipping)
    original = [b'waitforexitandrun', launcher, b'bytes-\xff\x80', b'--steam-vulkan']

    def run(args, expected, appid='3513350', extra_env=None, flag=b'--steam-wrap'):
        log.unlink(missing_ok=True)
        run_env = {**env, 'SteamAppId': appid}
        if extra_env:
            run_env.update(extra_env)
        result = subprocess.run([os.fsencode(binary), flag, os.fsencode(fake), *args],
                                env=run_env, capture_output=True, timeout=20)
        assert result.returncode == 37, (result.returncode, result.stderr)
        invoked = json.loads(log.read_text())
        assert invoked['arguments'] == [arg.hex() for arg in expected], (invoked, expected)
        assert invoked['appid'] == appid and invoked['compat'] == str(root / 'compat')

    run(original, original)
    run(original, [original[0], shipping_arg, *original[2:], b'-krqlv=hd'], flag=b'--steam-wrap-direct')
    run(original, original, flag=b'--steam-wrap-direct', extra_env={'ECHO_OFFICIAL_LAUNCHER': '1'})
    run(original, [original[0], shipping_arg, *original[2:], b'-krqlv=hd'], extra_env={'ECHO_DIRECT': '1'})
    run(original, original, extra_env={'ECHO_DIRECT': '1', 'ECHO_OFFICIAL_LAUNCHER': '1'})
    run(original + [b'--echo-native-direct'], [original[0], shipping_arg, *original[2:], b'-krqlv=hd'])
    run(original + [b'-krqlv=uhd', b'--echo-native-direct'], original + [b'-krqlv=uhd'])
    run(original + [b'--echo-native-direct', b'--echo-native-direct'], original)
    run(original + [b'--echo-native-direct'], original, appid='12345')
    config.write_text('{"bundles":{"HD":{"version":"3.8.0"},"UHD":{"version":"3.8.0"}}}')
    run(original + [b'--echo-native-direct'], original)
    result = subprocess.run([str(binary), '--steam-wrap'], env=env, capture_output=True, timeout=20)
    assert result.returncode == 2, (result.returncode, result.stderr)
    print('PASS: --steam-wrap exec, exact bytes and environment, ordinary Steam, isolated direct and fallback cases; no GUI/Vulkan or game started')
PY
