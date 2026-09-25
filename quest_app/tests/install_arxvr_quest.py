"""Interactively install and launch the development APK on one authorized headset."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
tools = Path(os.environ.get('ARXVR_TOOLCHAIN_ROOT', Path.home() / '.arxvr_toolchain'))
adb = Path(os.environ.get('ANDROID_HOME', tools / 'android-sdk')) / 'platform-tools/adb'
record_path = ROOT / 'dist/workspace-build.json'
if not record_path.exists(): record_path = ROOT / 'dist/build.json'
record = json.loads(record_path.read_text())
apk = ROOT / 'dist' / record['file']
if not adb.exists() or not apk.exists():
    raise SystemExit('Build the APK and set up the Android toolchain first.')
if hashlib.sha256(apk.read_bytes()).hexdigest() != record['sha256']:
    raise SystemExit('APK differs from the verified build. Rebuild before installing.')
listing = subprocess.check_output([str(adb), 'devices'], text=True)
devices = [line.split()[0] for line in listing.splitlines()[1:] if len(line.split()) == 2 and line.split()[1] == 'device']
if not devices:
    print(listing)
    raise SystemExit('Connect Quest by USB, enable developer mode and allow USB debugging inside the headset.')
for index, device in enumerate(devices, 1):
    model = subprocess.check_output([str(adb), '-s', device, 'shell', 'getprop', 'ro.product.model'], text=True).strip()
    print(f'{index}. {model} ({device})')
try:
    choice = int(input('Device number: ')) - 1
    if choice < 0 or choice >= len(devices): raise ValueError
except ValueError:
    raise SystemExit('No valid device selected.')
if input('Install or update ArX VR and launch it? (y/n): ').strip().lower() != 'y':
    raise SystemExit('No changes made.')
command = [str(adb), '-s', devices[choice]]
subprocess.run(command + ['install', '-r', str(apk)], check=True)
subprocess.run(command + ['shell', 'am', 'start', '-n', 'ai.arvolve.arxvr/' + record.get('activity', 'android.app.NativeActivity')], check=True)
print('Open ArX VR in Unknown Sources. The workspace build opens its connection guide.')
if input('Save startup diagnostics after you have tried it? (y/n): ').strip().lower() == 'y':
    output = subprocess.check_output(command + ['logcat', '-d', '-t', '1000', '-s', 'ArXVR:*', 'OpenXR-Loader:*', 'moonlight:*', 'moonlight-xr:*', 'AndroidRuntime:E'], text=True)
    path = ROOT / 'dist/headset-log.txt';path.write_text(output);print(path)
