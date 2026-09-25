"""Build the local Mac companion without modifying Sunshine or OpenWispr."""
from pathlib import Path
import os
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
# Installed off Google Drive: Drive sync mangles bundles, and TCC grants need a stable local path.
APP = Path.home() / 'Applications/ArX VR Bridge.app'
CACHE = Path.home() / 'Library/Caches/arxvr-build/clang-cache'

def signing_identity():
    # Real cert keeps a stable Team ID, so the Accessibility grant survives rebuilds. Ad-hoc resets it.
    if os.environ.get('ARXVR_SIGNING_IDENTITY'):
        return os.environ['ARXVR_SIGNING_IDENTITY']
    out = subprocess.run(['security', 'find-identity', '-v', '-p', 'codesigning'],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if 'Apple Development' in line or 'Developer ID Application' in line:
            return line.split()[1]
    print('WARNING: no signing cert found, falling back to ad-hoc. Accessibility will reset on every rebuild.')
    return '-'

# The ArX VR mark (the arch over the orb), for the window header and the Dock icon
LOGO = ROOT / 'macos/arxvr_icon.png'

def brand_assets(resources):
    """The window's logo, and a Dock icon drawn as the standard macOS rounded square in black:
    any other shape gets set on a grey tile by macOS."""
    from PIL import Image, ImageDraw
    resources.mkdir(parents=True, exist_ok=True)
    logo = Image.open(LOGO).convert('RGBA')
    logo.resize((256, 256), Image.LANCZOS).save(resources / 'logo.png')
    # Apple's icon grid: an 824 px rounded square inside the 1024 canvas, the rest transparent
    shape = Image.new('L', (1024, 1024), 0)
    ImageDraw.Draw(shape).rounded_rectangle((100, 100, 923, 923), 185, fill=255)
    # Black rounded square, the mark at 75% of it so it clears the edges
    tile = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
    tile.paste((0, 0, 0, 255), (0, 0, 1024, 1024), shape)
    tile.alpha_composite(logo.resize((618, 618), Image.LANCZOS), (203, 203))
    iconset = CACHE.parent / 'AppIcon.iconset'
    shutil.rmtree(iconset, ignore_errors=True); iconset.mkdir(parents=True)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            name = f'icon_{size}x{size}' + ('@2x' if scale == 2 else '') + '.png'
            tile.resize((size * scale, size * scale), Image.LANCZOS).save(iconset / name)
    subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(resources / 'AppIcon.icns')], check=True)

def main():
    contents = APP / 'Contents'
    binary = contents / 'MacOS/ArxMacHost'
    binary.parent.mkdir(parents=True, exist_ok=True)
    cache = CACHE; cache.mkdir(parents=True, exist_ok=True)
    subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-fmodules', f'-fmodules-cache-path={cache}',
        '-mmacosx-version-min=14.0', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
        '-framework', 'Cocoa', '-framework', 'ScreenCaptureKit', '-framework', 'CoreMedia',
        '-framework', 'CoreVideo', '-framework', 'ApplicationServices',
        '-framework', 'VideoToolbox', '-framework', 'AVFoundation',
        str(ROOT / 'macos/ArxMacHost.m'), str(ROOT / 'macos/ArxVideo.m'), str(ROOT / 'macos/ArxMirror.m'),
        '-o', str(binary)], check=True)
    subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Foundation',
        str(ROOT / 'macos/ArxVRLauncher.m'), '-o', str(contents / 'MacOS/ArxVRLauncher')], check=True)
    (contents / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleIdentifier': 'ai.arvolve.arxvr.macbridge', 'CFBundleExecutable': 'ArxVRLauncher',
        'CFBundleName': 'ArX VR Bridge', 'CFBundlePackageType': 'APPL', 'CFBundleVersion': '3',
        'CFBundleShortVersionString': '0.3.0', 'LSMinimumSystemVersion': '14.0',
        'NSHighResolutionCapable': True, 'CFBundleIconFile': 'AppIcon',
        'ArxPython': sys.executable,
        'ArxBridgeEntry': str(ROOT / 'arxvr_voice_bridge.py'),
        'NSScreenCaptureUsageDescription': 'Show the extra Mac desktops you create inside your Quest workspace.'}))
    shutil.copytree(ROOT / 'licenses', contents / 'Resources/licenses', dirs_exist_ok=True)
    brand_assets(contents / 'Resources')
    identity = signing_identity()
    # --options runtime is what makes the permissions stick. Without the hardened runtime macOS
    # keys Screen Recording on the binary's hash, so every rebuild silently revokes the grant
    # while the checkbox still reads as on. With it, the grant is keyed on identifier and team.
    subprocess.run(['codesign', '--force', '--options', 'runtime', '--sign', identity,
                    '--identifier', 'ai.arvolve.arxvr.machost', str(binary)], check=True)
    subprocess.run(['codesign', '--force', '--options', 'runtime', '--sign', identity, str(APP)], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(APP)], check=True)
    print(APP, '| signed with', identity)

if __name__ == '__main__': main()
