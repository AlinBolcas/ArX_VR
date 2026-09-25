"""Turns a 360 picture and its depth into a world you can stand in.

The headset already loads a baked model and the picture painted on it, so a world is
a dome whose every vertex is pushed out along its own direction by the depth. One
layer deep: you get parallax when you lean, and you cannot look behind anything.

Depth arrives in one of three kinds:
    'metres'     measured, from blender_world.py: used exactly as it is
    'depth'      distance, but at whatever scale the model guessed
    'disparity'  inverse depth, white near, 0 to 1 (Depth Anything V2, ZoeDepth)
Anything not measured is put at human scale the same way: the floor straight below
you is set at seated eye height, and near your feet it becomes a true flat floor.
That is what makes different models comparable, and what makes a world feel the
right size in the headset.

Output is a folder holding model.room and texture.png, ready for
`arxvr.py` -> "Put a world on the headset".

    MXR1 = magic, vertex count (u32), index count (u32),
           then 8 floats a vertex: position, normal, texture coordinate,
           then the indices as u16.
"""
from pathlib import Path
import struct

import numpy as np
from PIL import Image

# Where worlds are built: off the synced Drive folder, since one splat world's working
# files run to hundreds of megabytes. The ones worth shipping get copied into the repo.
BUILT = Path.home() / 'arxVR_worlds'

# Vertices are indexed with 16 bits, so the grid has to stay under 65535. 288 around
# by 160 up is 46,529 vertices, a little over one quad per degree.
SEGMENTS = 288
RINGS = 160
# Where your eyes are above the floor, sitting at a desk
EYE_HEIGHT_M = 1.3
# Nothing is let closer than this or further than that: closer is inside your head,
# further only costs precision
MIN_M = 0.4
MAX_M = 80.0
# Disparity is only relative, so it is read as spanning this range before the floor
# sets the true scale
DISPARITY_NEAR = 1.0
DISPARITY_FAR = 30.0
# Within this many degrees of straight down the floor is a flat plane: a model's
# guess about the ground under your feet is the first thing that reads as wrong
FLOOR_PLANE_DEG = 35.0
FLOOR_BLEND_DEG = 15.0


def ask(prompt, default=None):
    answer = input(f'{prompt}{f" [{default}]" if default else ""}: ').strip().strip('"\'')
    return answer or (default or '')


def resample(array, width, height):
    """Any depth array at the grid's size."""
    return np.asarray(Image.fromarray(np.asarray(array, dtype=np.float32), mode='F')
                      .resize((width, height), Image.BILINEAR), dtype=np.float32)


def load_depth(path, width, height):
    """A depth picture as 0 to 1, black to white, at the grid's size. What white means is the caller's to say."""
    depth = resample(np.asarray(Image.open(path).convert('F'), dtype=np.float32), width, height)
    low, high = np.percentile(depth, 1), np.percentile(depth, 99)
    if high - low < 1e-6:
        raise SystemExit('That depth map is flat: every pixel is the same distance.')
    return np.clip((depth - low) / (high - low), 0.0, 1.0)


def load_metres(path, width, height):
    """A depth_m.npy from blender_world.py: real distances, at the grid's size."""
    return resample(np.load(path), width, height)


def grid_angles():
    lon = np.linspace(-np.pi, np.pi, SEGMENTS + 1, dtype=np.float32)
    lat = np.linspace(0.0, np.pi, RINGS + 1, dtype=np.float32)
    return np.meshgrid(lon, lat)


def distances(depth, kind):
    """What the numbers mean in distance, before any scale is put on them."""
    if kind == 'disparity':
        inverse = 1.0 / DISPARITY_FAR + (1.0 / DISPARITY_NEAR - 1.0 / DISPARITY_FAR) * np.clip(depth, 0.0, 1.0)
        return 1.0 / inverse
    return np.maximum(np.asarray(depth, dtype=np.float32), 1e-3)


def floor_height(distance, lat_grid):
    """How far below the eye the floor is, read from the cap looking straight down."""
    down = -np.cos(lat_grid)
    cap = lat_grid > np.radians(180.0 - 20.0)
    return float(np.median(distance[cap] * down[cap]))


def human_scale(distance, lat_grid, eye_height):
    """Scaled so the floor sits at eye height, then a true plane near your feet."""
    distance = distance * (eye_height / max(floor_height(distance, lat_grid), 1e-6))
    down = -np.cos(lat_grid)
    from_down = np.degrees(np.pi - lat_grid)
    plane = eye_height / np.maximum(down, 1e-3)
    weight = np.clip((FLOOR_PLANE_DEG + FLOOR_BLEND_DEG - from_down) / FLOOR_BLEND_DEG, 0.0, 1.0)
    return distance * (1.0 - weight) + plane * weight


def build(depth, kind, eye_height=EYE_HEIGHT_M):
    """A dome of positions and texture coordinates, in metres around the viewer."""
    lon_grid, lat_grid = grid_angles()
    # The direction each vertex looks out along, in the headset's axes: -z ahead, y up,
    # longitude running the whole way round with the seam behind you
    sin_lat = np.sin(lat_grid)
    direction = np.stack([sin_lat * np.sin(lon_grid), np.cos(lat_grid), -sin_lat * np.cos(lon_grid)], axis=-1)

    distance = distances(depth, kind)
    if kind != 'metres':
        distance = human_scale(distance, lat_grid, eye_height)
    distance = np.clip(distance, MIN_M, MAX_M)
    # The seam has to meet itself exactly, or there is a crack down the back of the world
    distance[:, -1] = distance[:, 0]
    # And the poles are one point, however many vertices sit on them
    distance[0, :] = distance[0, :].mean()
    distance[-1, :] = distance[-1, :].mean()

    position = direction * distance[..., None]
    u = (lon_grid + np.pi) / (2.0 * np.pi)
    v = lat_grid / np.pi
    return position.astype(np.float32), np.stack([u, v], axis=-1).astype(np.float32)


def normals(position):
    """Inward facing normals from the surface itself, so the screen's light lands on it."""
    du = np.gradient(position, axis=1)
    dv = np.gradient(position, axis=0)
    n = np.cross(du, dv)
    n = n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    # Point them at the viewer: a world is seen from inside
    outward = np.sum(n * position, axis=-1, keepdims=True)
    return np.where(outward > 0, -n, n).astype(np.float32)


def faces():
    """Two triangles a quad, wound so the inside of the dome is the front."""
    rows = np.arange(RINGS)[:, None]
    cols = np.arange(SEGMENTS)[None, :]
    top_left = (rows * (SEGMENTS + 1) + cols).astype(np.uint32)
    top_right = top_left + 1
    bottom_left = top_left + (SEGMENTS + 1)
    bottom_right = bottom_left + 1
    quads = np.stack([top_left, bottom_left, top_right, top_right, bottom_left, bottom_right], axis=-1)
    return quads.reshape(-1).astype(np.uint16)


def write_world(folder, position, normal, uv, picture):
    folder.mkdir(parents=True, exist_ok=True)
    verts = np.concatenate([position.reshape(-1, 3), normal.reshape(-1, 3), uv.reshape(-1, 2)], axis=1)
    count = verts.shape[0]
    if count > 65535:
        raise SystemExit(f'{count} vertices is past the 65535 the format allows.')
    indices = faces()
    model = folder / 'model.room'
    with model.open('wb') as out:
        out.write(b'MXR1')
        out.write(struct.pack('<II', count, indices.size))
        out.write(verts.astype('<f4').tobytes())
        out.write(indices.astype('<u2').tobytes())
    # 4096 across is what the headset's texture upload is happy with
    if picture.width > 4096:
        picture = picture.resize((4096, picture.height * 4096 // picture.width), Image.LANCZOS)
    picture.convert('RGB').save(folder / 'texture.png')
    return model, count, indices.size


def make(folder, picture, depth, kind, eye_height=EYE_HEIGHT_M):
    """The whole step: depth at any size in, a finished world folder out."""
    grid = resample(depth, SEGMENTS + 1, RINGS + 1) if kind != 'disparity' else \
        np.clip(resample(depth, SEGMENTS + 1, RINGS + 1), 0.0, 1.0)
    position, uv = build(grid, kind, eye_height)
    return write_world(Path(folder), position, normals(position), uv, picture)


def main():
    print('Make a world from a 360 picture and its depth.\n')
    picture_path = Path(ask('360 picture (equirectangular jpg or png)')).expanduser()
    if not picture_path.is_file():
        raise SystemExit('No picture there.')
    depth_path = Path(ask('Depth for it (an image, or depth_m.npy from blender_world.py)')).expanduser()
    if not depth_path.is_file():
        raise SystemExit('No depth there.')
    name = ask('Name for the world', picture_path.stem)
    grid = (SEGMENTS + 1, RINGS + 1)
    if depth_path.suffix.lower() == '.npy':
        kind, depth = 'metres', load_metres(depth_path, *grid)
    else:
        print('What kind of depth picture is it?')
        print('  1. Disparity, white is near (Depth Anything V2, ZoeDepth)')
        print('  2. Distance, white is far')
        kind = 'depth' if ask('Choose', '1') == '2' else 'disparity'
        depth = load_depth(depth_path, *grid)
        if kind == 'depth':
            # A 0 to 1 picture of distance has lost where zero is; start it a little way out
            depth = 0.1 + depth

    picture = Image.open(picture_path)
    if abs(picture.width / picture.height - 2.0) > 0.05:
        print(f'  Careful: {picture.width}x{picture.height} is not 2:1, so it is probably not a 360 picture.')
    folder = BUILT / name
    model, count, index_count = make(folder, picture, depth, kind)
    print(f'\n{folder}')
    print(f'  {count} vertices, {index_count // 3} triangles, {model.stat().st_size // 1024} KB')
    print('  Put it on the headset with arxvr.py, option "Put a world on the headset".')


if __name__ == '__main__':
    main()
