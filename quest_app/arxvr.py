"""ArX VR control panel: start a session, build, install, see the headset and check status from one place."""
from datetime import datetime
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
# The Android toolchain tests/setup_arxvr_toolchain.py installs; an adb already on the PATH does too
TOOLS = Path(os.environ.get('ARXVR_TOOLCHAIN_ROOT', Path.home() / '.arxvr_toolchain'))
ADB = TOOLS / 'android-sdk/platform-tools/adb'
if not ADB.is_file() and shutil.which('adb'):
    ADB = Path(shutil.which('adb'))
BRIDGE = Path.home() / 'Applications/ArX VR Bridge.app'
STATUS = ROOT / 'host_tools/dist/bridge-status.json'
RECORD = ROOT / 'dist/workspace-build.json'
CAPTURES = ROOT / 'dist/headset'
PACKAGE = 'ai.arvolve.arxvr'


def adb(*args, timeout=30, binary=False):
    # stdin=DEVNULL: adb shell otherwise swallows piped input meant for this script
    return subprocess.run([str(ADB), *args], stdin=subprocess.DEVNULL, capture_output=True,
                          text=not binary, timeout=timeout)


def running(pattern):
    return subprocess.run(['pgrep', '-f', pattern], capture_output=True).returncode == 0


def quest_serial():
    lines = adb('devices').stdout.splitlines()[1:]
    ready = [l.split()[0] for l in lines if l.endswith('\tdevice')]
    return ready[0] if ready else None


def bridge_status():
    live = running('arxvr_voice_bridge.py')
    s = json.loads(STATUS.read_text()) if live and STATUS.exists() else {}
    s['running'] = live
    return s


def status():
    s = bridge_status()
    serial = quest_serial()
    dump = adb('shell', 'dumpsys', 'package', PACKAGE).stdout if serial else ''
    version = next((l.split('=')[1] for l in dump.splitlines() if 'versionName=' in l), 'not installed')
    ok = lambda v: 'yes' if v is True or v == 1 else 'NO'
    video = s.get('video') or {}
    print(f"  Bridge running        {ok(s.get('running'))}")
    print(f"  Screen recording      {ok(s.get('screen_recording'))}")
    print(f"  Mac control           {ok(s.get('accessibility'))}")
    print(f"  Desktop video         {ok(video.get('running'))}"
          + (f"  {video.get('width')}x{video.get('height')} {video.get('fps')}fps,"
             f" {video.get('clients')} viewing" if video.get('running') else ''))
    print(f"  Quest on USB          {ok(bool(serial))}")
    print(f"  ArX VR on headset     {version}")
    return s, serial


def wake():
    # Only wakes the display. It never tells the headset to stay awake off your head: that
    # left the app running and driving the Mac while nobody was wearing it.
    adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')


WHISPER = Path('/opt/homebrew/bin/whisper-cli')
WHISPER_MODEL = Path.home() / 'Library/Application Support/ArX VR Bridge/models/ggml-small.en.bin'
WHISPER_URL = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin'


def dictation_ready():
    # The bridge also takes an OpenWispr model, so an OpenWispr user is already set up
    binary = shutil.which('whisper-cli') or WHISPER.is_file()
    model = WHISPER_MODEL.is_file() or any((Path.home() / '.config/open-wispr/models').glob('ggml-*.bin'))
    return bool(binary and model)


def setup_dictation():
    if not (shutil.which('whisper-cli') or WHISPER.is_file()):
        brew = shutil.which('brew') or '/opt/homebrew/bin/brew'
        if not Path(brew).is_file():
            return print('Dictation needs Homebrew (brew.sh) to install whisper.cpp. Skipped.')
        subprocess.run([brew, 'install', 'whisper-cpp'], check=True)
    if not WHISPER_MODEL.is_file():
        # Into a .part first, so a cut download never passes for a model
        WHISPER_MODEL.parent.mkdir(parents=True, exist_ok=True)
        part = WHISPER_MODEL.with_suffix('.part')
        subprocess.run(['curl', '-L', '--fail', '-o', str(part), WHISPER_URL], check=True)
        part.rename(WHISPER_MODEL)
    print('Local dictation is ready. Restart the bridge if it is running.')


def dictation():
    if dictation_ready(): return print('Local dictation is already set up.')
    if input('Install whisper.cpp and download the model? (y/n): ').strip().lower() == 'y': setup_dictation()


def first_run():
    # A fresh clone has none of these yet: each is built once, on this Mac, after asking
    steps = [((TOOLS / 'android-sdk').is_dir() or RECORD.is_file(), 'Android toolchain (about 3 GB download, ~/.arxvr_toolchain)', 'tests/setup_arxvr_toolchain.py'),
             (RECORD.is_file(), 'Quest app (a few minutes)', 'tests/build_arxvr_workspace.py'),
             (BRIDGE.exists(), 'Mac bridge (~/Applications/ArX VR Bridge.app)', 'host_tools/build_arxvr_mac.py')]
    missing = [(what, script) for done, what, script in steps if not done]
    if not missing: return True
    print('First run. ArX VR still needs, one time:')
    for what, _ in missing: print(f'  - {what}')
    if input('Build them now? (y/n): ').strip().lower() != 'y': return False
    for _, script in missing: build(script)
    if not dictation_ready() and input('Also set up local dictation (whisper.cpp from Homebrew, a 490 MB speech model)? (y/n): ').strip().lower() == 'y':
        setup_dictation()
    return True


def start_session():
    if not first_run(): return print('Nothing started.')
    if not running('arxvr_voice_bridge.py'):
        print('Starting ArX VR Bridge...'); subprocess.run(['open', str(BRIDGE)])
    print('Waiting for the Quest on USB...')
    for _ in range(40):
        if quest_serial(): break
        time.sleep(3)
    else:
        return print('No Quest found. Plug in the cable and accept USB debugging inside the headset.')
    # Never launch a stale app: install the latest verified build if the headset is behind
    dump = adb('shell', 'dumpsys', 'package', PACKAGE).stdout
    installed = next((l.split('=')[1].strip() for l in dump.splitlines() if 'versionName=' in l), '')
    latest = json.loads(RECORD.read_text())['version']
    if installed != latest:
        print(f'Headset has {installed or "no app"}, installing {latest}...')
        install()
    # The bridge hands the headset its token and the Mac's address; launching before that means no control
    for _ in range(15):
        if bridge_status().get('quest_connected') is True: break
        time.sleep(1)
    wake()
    adb('shell', 'am', 'start', '-n', f'{PACKAGE}/com.limelight.ArxSession')
    if locked():
        print('ArX VR is waiting behind the headset LOCK SCREEN: put it on and unlock, it opens straight after.\n')
    else:
        print('ArX VR launched. Put the headset on.\n')
    s, _ = status()
    if not (s.get('screen_recording') is True and s.get('accessibility') is True):
        print('\n  Mac permissions missing for ArX VR Bridge. Stop (6) and start again after granting.')


def locked():
    # A locked headset shows its lock screen as an OS dialog and silently holds every app launch
    return 'mIsShowing=true' in adb('shell', 'dumpsys', 'window', 'policy').stdout


def capture(path):
    # screencap and scrcpy only see black on Quest; the system capture service sees the real compositor view
    shots = '/sdcard/Oculus/Screenshots/'
    before = set(adb('shell', 'ls', shots).stdout.split())
    adb('shell', 'am', 'startservice', '-n', 'com.oculus.metacam/.capture.CaptureService',
        '-a', 'TAKE_SCREENSHOT', '--ei', 'screenshot_height', '1080', '--ei', 'screenshot_width', '1920')
    for _ in range(20):
        time.sleep(0.5)
        new = set(adb('shell', 'ls', shots).stdout.split()) - before
        if new:
            name = sorted(new)[-1]
            time.sleep(0.5)
            adb('pull', shots + name, str(path))
            adb('shell', 'rm', shots + name)
            return True
    return False


def headset_view():
    # What the headset shows right now plus the app's recent log, saved side by side
    if not quest_serial():
        return print('No Quest on USB.')
    if locked():
        print('  Headset is LOCKED: put it on and unlock it. Launches wait behind the lock screen.')
    CAPTURES.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    shot = CAPTURES / f'view_{stamp}.jpg'
    if not capture(shot):
        print('  Screenshot failed: the headset may be asleep.')
    log = CAPTURES / f'log_{stamp}.txt'
    log.write_text(adb('logcat', '-d', '-t', '800', '-s', 'ArXVR:*', 'moonlight:*', 'moonlight-common-c:*',
                       'com.limelight.LimeLog:*', 'moonlight-xr:*', 'AndroidRuntime:E').stdout)
    if shot.exists():
        print(f'  Screenshot  {shot}  ({shot.stat().st_size // 1024} KB)')
    print(f'  Log         {log}')


def build(script):
    subprocess.run([sys.executable, str(ROOT / script)], check=True)


def install():
    record = json.loads(RECORD.read_text())
    apk = ROOT / 'dist' / record['file']
    if hashlib.sha256(apk.read_bytes()).hexdigest() != record['sha256']:
        return print('APK differs from the verified build. Rebuild it first (option 3).')
    if not quest_serial():
        return print('No Quest on USB.')
    print(f"Installing {apk.name} (pairing is kept)...")
    result = adb('install', '-r', str(apk), timeout=300)
    print((result.stdout.strip().splitlines() or [result.stderr])[-1])


def stop_session():
    subprocess.run(['pkill', '-TERM', '-f', 'arxvr_voice_bridge.py'])
    time.sleep(1)
    subprocess.run(['pkill', '-TERM', '-f', 'ArX VR Bridge.app/Contents/MacOS'])
    adb('shell', 'am', 'force-stop', PACKAGE)
    adb('shell', 'am', 'broadcast', '-a', 'com.oculus.vrpowermanager.automation_disable')
    print('Bridge stopped, desktop video off, extra displays removed, ArX VR closed.')


def send_file(source, folder, name=None):
    # Streamed straight into the app's own storage: the app cannot read files dropped on
    # /sdcard (Android's storage sandbox), which is how the first version made empty worlds
    name = name or source.name
    adb('shell', 'run-as', PACKAGE, 'mkdir', '-p', folder)
    with open(source, 'rb') as data:
        sent = subprocess.run([str(ADB), 'exec-in', f"run-as {PACKAGE} sh -c 'cat > {folder}/{name}'"],
                              stdin=data, capture_output=True, timeout=300)
    # The device can still be writing the tail when the pipe closes, so the size is
    # read a few times before the copy is called short
    for _ in range(10):
        size = adb('shell', 'run-as', PACKAGE, 'stat', '-c', '%s', f'{folder}/{name}').stdout.strip()
        if size == str(source.stat().st_size):
            break
        time.sleep(0.5)
    if sent.returncode or size != str(source.stat().st_size):
        return print(f'{source.name} did not arrive whole. Is this a debug build of ArX VR?') or False
    return True


WORLDS = Path.home() / 'arxVR_worlds'


def add_world():
    # A splat world folder (world.splat and its tile), without rebuilding the app
    if not quest_serial():
        return print('No Quest on USB.')
    ready = sorted(p.parent for p in WORLDS.glob('*/world.splat'))
    for i, folder in enumerate(ready, 1):
        print(f'{i}. {folder.name}  ({(folder / "world.splat").stat().st_size // 32:,} splats)')
    given = input('Which world (number, or a folder): ').strip().strip('"\'')
    source = ready[int(given) - 1] if given.isdigit() and 1 <= int(given) <= len(ready) else Path(given).expanduser()
    splat = source / 'world.splat'
    if not splat.is_file():
        return print('A world folder holds world.splat (make one with splat_convert.py or marble_world.py).')
    folder = f'files/worlds/{source.name}'
    # The picker tile, the flat 360 used by the panorama toggle, and the music loop, whichever exist
    extras = [p for p in (source / n for n in ('texture.jpg', 'panorama.jpg', 'ambient.ogg')) if p.is_file()]
    if send_file(splat, folder) and all(send_file(p, folder) for p in extras):
        print(f'World "{source.name}" is on the headset.')
    # Where each world sits in the menu: one folder name a line, "-" for an empty cell
    layout = WORLDS / 'layout.txt'
    if layout.is_file() and send_file(layout, 'files/worlds'):
        print('Menu layout sent (~/arxVR_worlds/layout.txt).')
    print('Open the pie menu, choose World, and pick it.')
    print('Restart ArX VR first if it is already running: the list is read when it starts.')


def list_worlds():
    if not quest_serial():
        return print('No Quest on USB.')
    worlds = adb('shell', f"run-as {PACKAGE} sh -c 'ls files/worlds/*/world.splat'").stdout.split()
    names = [w.split('/')[-2] for w in worlds if w.endswith('world.splat')]
    print('Splat worlds: ' + (', '.join(names) or 'none') + '  (the picker shows the first four)')


MENU = [
    ('Start session', start_session),
    ('Status', status),
    ('Build Quest app', lambda: build('tests/build_arxvr_workspace.py')),
    ('Install Quest app on headset', install),
    ('Build Mac Bridge', lambda: build('host_tools/build_arxvr_mac.py')),
    ('Stop session', stop_session),
    ('Set up local dictation (whisper.cpp and a 490 MB model, asks first)', dictation),
    ('Headset view (screenshot and log)', headset_view),
    ('Make a splat world with Marble (World Labs credits, asks first)', lambda: build('host_tools/worlds/marble_world.py')),
    ('Turn a splat file into a headset world', lambda: build('host_tools/worlds/splat_convert.py')),
    ('Merge a splat world for a lighter headset (asks the angle)', lambda: build('host_tools/worlds/splat_merge.py')),
    ('Check a splat world on the Mac (headset shaders beside Spark)', lambda: build('host_tools/worlds/splat_harness.py')),
    ('Put a world on the headset', add_world),
    ('Worlds on the headset', list_worlds),
]

if __name__ == '__main__':
    for i, (label, _) in enumerate(MENU, 1):
        print(f'{i}. {label}')
    choice = input('Choose (Enter = 1): ').strip() or '1'
    if not choice.isdigit() or not 1 <= int(choice) <= len(MENU):
        raise SystemExit('No valid option.')
    MENU[int(choice) - 1][1]()
