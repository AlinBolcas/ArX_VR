"""The headset's splat renderer on the Mac, beside Spark, before any headset is plugged in.

The page draws one .splat twice from the same camera:
    left   the headset's own shaders, read out of xr_splat_shaders.h, fed the texels the
           headset's own C packs (xr_splat_data.c, built here with clang)
    right  Spark 2.2, the reference web renderer
and window.compare() walks six fixed views and scores how far apart the two are.

    python3 splat_harness.py      pick a world, then open the address it prints

It serves the page from ~/arxVR_worlds/_harness until Ctrl+C, and nothing after that.
"""
from pathlib import Path
import http.server
import json
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
APP = HERE.parents[1]
JNI = APP / 'streaming/src/main/jni/xr-renderer'
HARNESS = Path.home() / 'arxVR_worlds' / '_harness'
PORT = 8766


def shader_sources():
    """The two shader strings, exactly as the C compiler sees them."""
    text = (JNI / 'xr_splat_shaders.h').read_text()
    out = {}
    for name in ('SPLAT_VERTEX_SRC', 'SPLAT_FRAGMENT_SRC'):
        body = text[text.index(name + ' ='):]
        body = body[:body.index(';\n')]
        # Comments between the literals are C's, not the shader's
        body = re.sub(r'//[^\n]*', '', body)
        out[name] = ''.join(json.loads('"' + s + '"') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    return out['SPLAT_VERTEX_SRC'], out['SPLAT_FRAGMENT_SRC']


def packer():
    """The headset's C packing, built for this Mac."""
    binary = HARNESS / 'bin' / 'splat_data_test'
    binary.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['clang', '-O2', '-Wall', '-Werror', '-o', str(binary), str(APP / 'tests/splat_data_test.c'),
                    str(JNI / 'xr_splat_data.c'), '-lm'], check=True)
    return binary


def build(splat, reference=None):
    """The page, the packed texels and the file itself, ready to serve. With a reference,
    Spark draws that file instead, so a processed world is scored against its original."""
    splat = Path(splat)
    HARNESS.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(packer()), 'pack', str(splat), str(HARNESS / 'texels.bin')],
                            check=True, capture_output=True, text=True)
    count = int(result.stdout.split()[0])
    shutil.copyfile(reference or splat, HARNESS / 'world.splat')
    vertex, fragment = shader_sources()
    page = (HERE / 'splat_harness.html').read_text()
    page = page.replace('__VERTEX__', json.dumps(vertex)).replace('__FRAGMENT__', json.dumps(fragment))
    page = page.replace('__COUNT__', str(count)).replace('__NAME__', splat.parent.name if splat.stem == 'world' else splat.stem)
    (HARNESS / 'index.html').write_text(page)
    return count


def serve():
    handler = lambda *a: http.server.SimpleHTTPRequestHandler(*a, directory=str(HARNESS))
    with http.server.ThreadingHTTPServer(('127.0.0.1', PORT), handler) as server:
        print(f'  http://127.0.0.1:{PORT}/   (Ctrl+C stops it)')
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


def main():
    worlds = sorted(p for p in (Path.home() / 'arxVR_worlds').glob('*/**/*.splat') if '_harness' not in p.parts)
    if not worlds:
        raise SystemExit('No .splat files under ~/arxVR_worlds yet.')
    print('Which splat?')
    for i, p in enumerate(worlds, 1):
        print(f'  {i}. {p.relative_to(Path.home() / "arxVR_worlds")}  ({p.stat().st_size // 32:,} splats)')
    choice = input('Choose [1]: ').strip() or '1'
    splat = worlds[int(choice) - 1]
    print(f'  {build(splat):,} splats packed by the headset code')
    serve()


if __name__ == '__main__':
    sys.exit(main())
