"""Build the adopted streaming workspace, run tests and verify the signed APK."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(os.environ.get('ARXVR_TOOLCHAIN_ROOT', Path.home() / '.arxvr_toolchain'))
SDK = Path(os.environ.get('ANDROID_HOME', TOOLS / 'android-sdk'))
JAVA = Path(os.environ.get('JAVA_HOME', TOOLS / 'jdk-17/Contents/Home'))
ENV = dict(os.environ, JAVA_HOME=str(JAVA), ANDROID_HOME=str(SDK), GRADLE_USER_HOME=str(TOOLS / 'gradle-cache'))


def version():
    # Read from the build file, so bumping the app never means editing this script too
    for line in (ROOT / 'streaming/build.gradle').read_text().splitlines():
        if line.strip().startswith('versionName'):
            return line.split("'")[1]
    raise SystemExit('No versionName in streaming/build.gradle')


def main():
    subprocess.run([str(TOOLS / 'gradle-8.9/bin/gradle'), '--no-daemon', '--console=plain',
        ':streaming:assembleDebug', ':streaming:testDebugUnitTest', ':streaming:lintDebug'],
        cwd=ROOT, env=ENV, check=True)
    subprocess.run(['make', 'test'], cwd=ROOT / 'streaming/src/test/cpp', check=True)
    subprocess.run([sys.executable, str(ROOT / 'tests/arxvr_device_lab_test.py')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'tests/arxvr_bridge_test.py')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'tests/arxvr_world_test.py')], check=True)
    source = ROOT / 'streaming/build/outputs/apk/debug/streaming-debug.apk'
    with ZipFile(source) as apk:
        assert apk.testzip() is None
        names = set(apk.namelist())
        required = {'lib/arm64-v8a/libmoonlight-core.so', 'lib/arm64-v8a/libxr-renderer.so',
                    'lib/arm64-v8a/libopenxr_loader.so', 'assets/licenses/Moonlight-GPL-3.0.txt'}
        assert required <= names, required - names
        assert {n.split('/')[1] for n in names if n.startswith('lib/')} == {'arm64-v8a'}
    badging = subprocess.check_output([str(SDK / 'build-tools/35.0.0/aapt'), 'dump', 'badging', str(source)], text=True)
    release = version()
    for expected in ["name='ai.arvolve.arxvr'", f"versionName='{release}'", "name='com.limelight.ArxSession'", "targetSdkVersion:'35'"]:
        assert expected in badging, expected
    signed = subprocess.check_output([str(SDK / 'build-tools/35.0.0/apksigner'), 'verify', '--verbose', str(source)], env=ENV, text=True)
    dist = ROOT / 'dist'; dist.mkdir(exist_ok=True)
    target = dist / f'arxvr_v{release}_debug.apk'; shutil.copy2(source, target)
    record = {'version': release, 'application_id': 'ai.arvolve.arxvr', 'activity': 'com.limelight.ArxSession',
        'file': target.name, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'bytes': target.stat().st_size, 'signature_verified': True, 'device_tested': False,
        'upstream': 'Moonlight XR a33b8dbcce20092c25ba5f08caa1c905864b426b',
        'capabilities': ['own desktop video: ScreenCaptureKit and VideoToolbox on the Mac, MediaCodec here',
            'no pairing and no PIN: the bridge provisions address and token over the cable',
            'curved screen', 'hands/controllers', 'move/resize panel', 'in-space dock and guide',
            'spatial keyboard', 'USB local Whisper dictation', 'Mac input over the bridge', 'two extra Mac desktops'],
        'limits': ['Extra desktops use a 12 fps USB preview transport',
            'Mac companion needs Screen Recording and Accessibility permission', 'No Blender integration']}
    (dist / 'workspace-build.json').write_text(json.dumps(record, indent=2) + '\n')
    (dist / 'workspace-signature.txt').write_text(signed)
    (dist / 'workspace-apk-metadata.txt').write_text(badging)
    print(target)


if __name__ == '__main__': main()
