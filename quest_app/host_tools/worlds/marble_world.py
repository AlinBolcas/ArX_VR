"""A Gaussian splat world from Marble (World Labs), straight to a headset world.

    a sentence, a picture, a 360 panorama or a video
      -> Marble through worldlabs_client.py (the World API)
      -> its 500k SPZ (or the PLY export when the SPZ is a kind this cannot read)
      -> splat_convert.py: headset frame, metric, floor under you, splat budget
      -> ~/arxVR_worlds/<name>/world.splat + texture.jpg, ready for arxvr.py

Spends World Labs credits, so it says how many and asks first. The key is
WORLD_LABS_API_KEY, in the environment or a .env at the repo root.
"""
from pathlib import Path
import json
import sys

import make_world
import splat_convert
import worldlabs_client
KINDS = ['A sentence', 'A picture (file or URL), with a sentence if you like',
         'A 360 panorama (file or URL)', 'A video (file or URL)',
         'Several views of the same place, each with its angle (0 ahead, 90 right, 180 behind)']
# Rough spend a world, from World Labs' pricing: $1 buys 1250 credits
MODELS = [('marble-1.0-draft', 250), ('marble-1.1', 1600), ('marble-1.1-plus', 3000)]


def worldlabs():
    try:
        return worldlabs_client.WorldLabsAPI()
    except RuntimeError as e:
        raise SystemExit(str(e))


def fetch_splats(api, world, folder, tier='500k'):
    """The SPZ at this tier (100k, 150k, 500k or full_res), or the PLY export when the SPZ
    is a version this Mac cannot unpack."""
    assets = api.assets(world)
    spz = assets['spz'].get(tier) or assets['spz'].get('full_res')
    if spz:
        path = api.download(spz, folder / f'marble_{tier}.spz')
        try:
            splat_convert.read(path)
            return path
        except SystemExit as e:
            print(f'  {e}\n  Asking for the PLY export instead.')
    return api.download(api.export_ply(world['id']), folder / 'marble.ply')


def make(api, kind, source, text, model, name, views=None, tier='500k'):
    """views: [(azimuth, file or URL), ...] for several views of one place (kind 4)."""
    folder = make_world.BUILT / name / 'marble'
    folder.mkdir(parents=True, exist_ok=True)
    args = {'text': text or None, 'model': model, 'name': name}
    if views:
        args['images'] = views
    elif kind in (1, 2):
        args['image'] = source
    elif kind == 3:
        args['video'] = source
    operation = api.generate(**args)
    print(f'  started: operation {operation.get("operation_id")}, about 5 minutes')
    world = api.wait(operation['operation_id'], on_progress=lambda p, t: print(
        f'  {t / 60:4.1f} min  {p.get("status", "")} {p.get("description", "")}'.rstrip()))
    (folder / 'world.json').write_text(json.dumps(world, indent=2) + '\n')
    assets = api.assets(world)
    splats = fetch_splats(api, world, folder, tier)
    panorama = api.download(assets['panorama'], folder / 'panorama.jpg') if assets['panorama'] else None
    # What splat_convert.py needs to put the world the right size and the right way up
    (folder / 'marble.json').write_text(json.dumps({
        'name': name, 'metric_scale_factor': assets['metric_scale_factor'],
        'ground_plane_offset': assets['ground_plane_offset'],
        'panorama': str(panorama) if panorama else None, 'marble_url': assets['marble_url']}, indent=2) + '\n')
    return folder, splats, assets


def main():
    print('Make a Gaussian splat world with Marble (World Labs).\n')
    api = worldlabs()
    try:
        print(f'  Credits: {json.dumps(api.credits())}')
    except RuntimeError as e:
        print(f'  Could not read the balance ({e})')
    print('What to start from?')
    kind = splat_convert.choose('Choose', KINDS)
    source, views = None, None
    if kind == 4:
        views = []
        print('One view per line as angle and file, e.g. 0 front.png. An empty line ends it.')
        while (line := input('  view: ').strip()):
            angle, _, path = line.partition(' ')
            if not Path(path.strip().strip('"\'')).expanduser().is_file():
                raise SystemExit(f'No file at {path}.')
            views.append((float(angle), str(Path(path.strip().strip('"\'')).expanduser())))
    elif kind:
        source = make_world.ask('File or URL')
        if not source.startswith(('http://', 'https://')) and not Path(source).expanduser().is_file():
            raise SystemExit('No file there.')
        source = source if source.startswith('http') else str(Path(source).expanduser())
    text = make_world.ask('Describe the place' if kind == 0 else 'A sentence to steer it (Enter for none)', '')
    if kind == 0 and not text:
        raise SystemExit('A world from a sentence needs the sentence.')
    print('Which model?')
    model, spend = MODELS[splat_convert.choose('Choose', [f'{m}  (up to about {c} credits, ${c / 1250:.2f})'
                                                          for m, c in MODELS], 2)]
    first = source or (views[0][1] if views else 'world')
    name = make_world.ask('Name for the world', '_'.join((text or Path(first).stem).lower().split()[:3]))
    print('How many splats on the headset?')
    budget = splat_convert.BUDGETS[splat_convert.choose('Choose', [l for l, _ in splat_convert.BUDGETS])][1]
    if make_world.ask(f'This spends up to about {spend} credits (${spend / 1250:.2f}). Go ahead? (y/n)', 'n').lower() != 'y':
        raise SystemExit('Nothing spent.')

    folder, splats, assets = make(api, kind, source, text, model, name, views)
    world, found, kept = splat_convert.convert(splats, name, 'opencv', budget, assets['metric_scale_factor'],
                                               assets['ground_plane_offset'], folder / 'panorama.jpg'
                                               if (folder / 'panorama.jpg').is_file() else None)
    print(f'\n{world}')
    print(f'  {kept:,} of {found:,} splats on the headset')
    print(f'  On the web: {assets["marble_url"]}')
    print('  Check it on the Mac first: splat_harness.py. Then arxvr.py, "Put a world on the headset".')


if __name__ == '__main__':
    sys.exit(main())
