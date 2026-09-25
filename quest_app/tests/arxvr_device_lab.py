"""USB mirror, screenshot and diagnostic bundles for the Quest development loop."""
from datetime import datetime, timezone
from pathlib import Path
import json
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get('ARXVR_TOOLCHAIN_ROOT', Path.home() / '.arxvr_toolchain'))
ADB = TOOLS / 'android-sdk/platform-tools/adb'
SCRCPY = TOOLS / 'scrcpy-4.1/scrcpy'
PACKAGE = 'ai.arvolve.arxvr'


def run(args, timeout=20):
    return subprocess.run([str(a) for a in args], capture_output=True, timeout=timeout, check=False)


def device():
    result = run([ADB, 'devices'])
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace').strip())
    rows = [line.split() for line in result.stdout.decode().splitlines()[1:] if line.strip()]
    ready = [row[0] for row in rows if len(row) >= 2 and row[1] == 'device']
    if len(ready) != 1:
        raise RuntimeError('Connect exactly one authorized Quest over USB. Accept USB debugging inside the headset.')
    return ready[0]


def mirror(serial):
    if not SCRCPY.is_file():
        raise RuntimeError('Install the documented standalone scrcpy toolchain first.')
    env = dict(os.environ, ADB=str(ADB))
    return subprocess.call([str(SCRCPY), '--serial', serial, '--no-audio', '--no-control',
        '--max-size=1600', '--max-fps=30', '--window-title=ArX VR | Quest mirror'], env=env)


def decode_desktop_capture(metadata_path):
    from PIL import Image
    metadata_path = Path(metadata_path)
    meta = json.loads(metadata_path.read_text())
    width, height = meta['width'], meta['height']
    if not isinstance(width, int) or not isinstance(height, int) or not (0 < width <= 8192 and 0 < height <= 8192):
        raise ValueError('Invalid capture dimensions')
    if meta.get('format') != 'RGBA' or meta.get('origin') != 'bottom-left':
        raise ValueError('Unsupported native capture format')
    raw = metadata_path.with_suffix('.raw').read_bytes()
    if len(raw) != width * height * 4:
        raise ValueError('Incomplete capture: retry after the headset finishes saving')
    frame = Image.frombytes('RGBA', (width, height), raw).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    output = metadata_path.with_suffix('.png')
    frame.save(output)
    return output


def capture(serial, output=None):
    folder = Path(output) if output else ROOT / 'dist/diagnostics' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    folder.mkdir(parents=True, exist_ok=False)
    base = [ADB, '-s', serial]
    report = {'utc': datetime.now(timezone.utc).isoformat(), 'package': PACKAGE, 'files': [], 'notes': []}
    queries = {
        'app-log.txt': ['logcat', '-d', '-t', '6000', '-s', 'ArXVR:V', 'moonlight:V', 'LimeLog:V', 'XrRenderer:V', 'moonlight-xr:V', 'AndroidRuntime:E'],
        'package.txt': ['shell', 'dumpsys', 'package', PACKAGE],
        'power.txt': ['shell', 'dumpsys', 'power'],
    }
    for name, args in queries.items():
        result = run(base + args)
        content = result.stdout.decode(errors='replace')
        if name == 'power.txt':
            content = '\n'.join(line for line in content.splitlines() if any(x in line for x in ('mWakefulness=', 'mIsPowered=', 'mProximityPositive=')))
        if name == 'package.txt':
            content = '\n'.join(line for line in content.splitlines() if any(x in line for x in ('versionCode=', 'versionName=', 'firstInstallTime=', 'lastUpdateTime=', 'RECORD_AUDIO:', 'HAND_TRACKING:')))
        (folder / name).write_text(content)
        report['files'].append(name)
    result = run(base + ['exec-out', 'screencap', '-p'])
    if result.returncode == 0 and result.stdout.startswith(b'\x89PNG\r\n\x1a\n'):
        from PIL import Image
        import io
        with Image.open(io.BytesIO(result.stdout)) as frame:
            frame.verify()
        (folder / 'headset.png').write_bytes(result.stdout)
        report['files'].append('headset.png')
    else:
        report['notes'].append('No valid headset screenshot. The Quest may be asleep; retry while worn or use USB mirroring. No stale image reused.')
    for remote in [f'/sdcard/Android/data/{PACKAGE}/files/logs/arxvr.log', '/sdcard/Download/ArXVR/arxvr.log']:
        result = run(base + ['pull', remote, str(folder / 'session.log')])
        if result.returncode == 0:
            report['files'].append('session.log'); break
    remote_dir = f'/sdcard/Android/data/{PACKAGE}/files'
    result = run(base + ['shell', 'ls', '-1', remote_dir])
    names = [n for n in result.stdout.decode(errors='replace').splitlines() if re.fullmatch(r'cap_user_[a-zA-Z0-9_]+_source\.json', n)]
    for name in names[-10:]:
        for leaf in (name, name.replace('.json', '.raw')):
            result = run(base + ['pull', remote_dir + '/' + leaf, str(folder / leaf)])
            if result.returncode:
                report['notes'].append('Could not pull ' + leaf)
        try:
            image = decode_desktop_capture(folder / name)
            report['files'].append(image.name)
        except (OSError, ValueError, KeyError) as error:
            report['notes'].append(str(error))
    (folder / 'capture.json').write_text(json.dumps(report, indent=2) + '\n')
    print(folder)
    for note in report['notes']: print(note)
    return folder


def main():
    serial = device()
    print('1. Open live USB mirror\n2. Save screenshot and diagnostics\n3. Launch ArX VR workspace')
    choice = input('Choose [2]: ').strip() or '2'
    if choice == '1': mirror(serial)
    elif choice == '2': capture(serial)
    elif choice == '3':
        result = run([ADB, '-s', serial, 'shell', 'am', 'start', '-n', PACKAGE + '/com.limelight.ArxSession'])
        print(result.stdout.decode(errors='replace'))
    else: raise SystemExit('Unknown choice')


if __name__ == '__main__':
    try: main()
    except (RuntimeError, subprocess.TimeoutExpired) as error: raise SystemExit(str(error))
