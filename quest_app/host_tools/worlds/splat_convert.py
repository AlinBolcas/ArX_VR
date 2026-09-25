"""A Gaussian splat world for the headset, from whatever made it.

    in     .ply    the usual 3DGS layout: Marble's PLY export, SHARP, most trainers
           .spz    Niantic's format: v2 and v3 read as they are, v4 needs the zstandard package
           .splat  antimatter15
    out    ~/arxVR_worlds/<name>/world.splat   what the headset loads
           ~/arxVR_worlds/<name>/texture.jpg   its tile in the picker (the panorama if there is one)
           ~/arxVR_worlds/<name>/meta.json

Put into the headset's frame: metres, y up, -z ahead, your eyes at the origin and the
floor at seated eye height under them. Marble's files are OpenCV (y down, z ahead) and
come with their own metric scale and floor height; anything else has its floor found.
Then cut to a splat budget, dropping first what can least be seen from where you sit.
"""
from pathlib import Path
import gzip
import json
import math
import struct
import sys

import numpy as np
from PIL import Image

import make_world

SH_C0 = 0.28209479177387814
SPZ_MAGIC = 0x5053474E
SPZ_COLOR_SCALE = 0.15
# Nothing nearer than this to your eyes (it would fill them) or further than the headset draws
NEAR_M = 0.15
FAR_M = 150.0
BUDGETS = [('300k, Quest 2', 300_000), ('500k', 500_000), ('1M, Quest 3', 1_000_000), ('keep all', None)]


# ---------------------------------------------------------------- reading
# Every reader returns the same thing: xyz (n, 3), rgb and alpha 0..1, scale in metres
# (n, 3), rot as unit quaternions w x y z (n, 4), in whatever frame the file was in

PLY_TYPES = {'float': 'f4', 'float32': 'f4', 'double': 'f8', 'uchar': 'u1', 'uint8': 'u1', 'char': 'i1',
             'int8': 'i1', 'short': 'i2', 'int16': 'i2', 'ushort': 'u2', 'uint16': 'u2', 'int': 'i4',
             'int32': 'i4', 'uint': 'u4', 'uint32': 'u4'}


def read_ply(path):
    data = Path(path).read_bytes()
    end = data.index(b'end_header') + len(b'end_header')
    end += 2 if data[end:end + 2] == b'\r\n' else 1
    lines = data[:end].decode('ascii', 'replace').splitlines()
    if not any(l.startswith('format binary_little_endian') for l in lines):
        raise SystemExit('Only binary little endian PLY files are read.')
    count, fields, in_vertex = 0, [], False
    for line in lines:
        words = line.split()
        if words[:1] == ['element']:
            if in_vertex:
                break
            in_vertex = words[1] == 'vertex'
            count = int(words[2]) if in_vertex else count
            if not in_vertex and not fields:
                raise SystemExit('The vertices have to come first in the PLY.')
        elif words[:1] == ['property'] and in_vertex:
            fields.append((words[2], '<' + PLY_TYPES[words[1]]))
    v = np.frombuffer(data, dtype=np.dtype(fields), count=count, offset=end)
    names = v.dtype.names
    xyz = np.stack([v['x'], v['y'], v['z']], axis=1).astype(np.float32)
    if 'f_dc_0' in names:
        rgb = 0.5 + SH_C0 * np.stack([v['f_dc_0'], v['f_dc_1'], v['f_dc_2']], axis=1)
    else:
        rgb = np.stack([v['red'], v['green'], v['blue']], axis=1) / 255.0
    alpha = 1 / (1 + np.exp(-v['opacity'].astype(np.float64))) if 'opacity' in names else np.ones(count)
    scale = np.exp(np.stack([v['scale_0'], v['scale_1'], v['scale_2']], axis=1).astype(np.float64))
    rot = np.stack([v['rot_0'], v['rot_1'], v['rot_2'], v['rot_3']], axis=1).astype(np.float64)
    return tidy(xyz, rgb, alpha, scale, rot)


def spz_streams(path):
    """Header fields and the attribute buffers, whichever generation of the file it is."""
    raw = Path(path).read_bytes()
    if raw[:2] == b'\x1f\x8b':
        # v1 to v3: the whole thing gzipped, a 16 byte header, then the attributes end to end
        data = gzip.decompress(raw)
        magic, version, n, sh_degree, fractional, _flags, _ = struct.unpack_from('<IIIBBBB', data, 0)
        if magic != SPZ_MAGIC:
            raise SystemExit('Not an SPZ file.')
        sizes = [n * (6 if version == 1 else 9), n, 3 * n, 3 * n, n * (4 if version >= 3 else 3)]
        chunks, at = [], 16
        for size in sizes:
            chunks.append(data[at:at + size])
            at += size
        return version, n, fractional, chunks
    # v4: a 32 byte header in the clear, a table of contents, then one zstd stream per attribute
    magic, version, n, sh_degree, fractional, _flags, streams, toc = struct.unpack_from('<IIIBBBBI', raw, 0)
    if magic != SPZ_MAGIC:
        raise SystemExit('Not an SPZ file.')
    try:
        import zstandard
    except ImportError:
        raise SystemExit('This .spz is version 4, which is zstd compressed. Either install the zstandard '
                         'package (pip install zstandard, in its own venv) or use the PLY export instead.')
    at = toc + 16 * streams
    chunks = []
    for i in range(streams):
        packed, unpacked = struct.unpack_from('<QQ', raw, toc + 16 * i)
        chunks.append(zstandard.ZstdDecompressor().decompress(raw[at:at + packed], max_output_size=unpacked))
        at += packed
    return version, n, fractional, chunks


def read_spz(path):
    version, n, fractional, chunks = spz_streams(path)
    pos, alpha, color, scale, rot = chunks[:5]
    if version == 1:
        xyz = np.frombuffer(pos, '<f2').reshape(n, 3).astype(np.float32)
    else:
        b = np.frombuffer(pos, np.uint8).reshape(n, 3, 3).astype(np.int32)
        fixed = b[..., 0] | (b[..., 1] << 8) | (b[..., 2] << 16)
        fixed = np.where(fixed & 0x800000, fixed - (1 << 24), fixed)
        xyz = (fixed / float(1 << fractional)).astype(np.float32)
    a = np.frombuffer(alpha, np.uint8) / 255.0
    dc = (np.frombuffer(color, np.uint8).reshape(n, 3) / 255.0 - 0.5) / SPZ_COLOR_SCALE
    s = np.exp(np.frombuffer(scale, np.uint8).reshape(n, 3) / 16.0 - 10.0)
    if version >= 3:
        q = spz_smallest_three(np.frombuffer(rot, '<u4', count=n))
    else:
        xyz_q = np.frombuffer(rot, np.uint8).reshape(n, 3) / 127.5 - 1.0
        q = np.concatenate([xyz_q, np.sqrt(np.maximum(0, 1 - (xyz_q ** 2).sum(1)))[:, None]], axis=1)
    # SPZ keeps x y z w; everything in here is w x y z
    return tidy(xyz, 0.5 + SH_C0 * dc, a, s, q[:, [3, 0, 1, 2]])


def spz_smallest_three(comp):
    """The largest component's index in the top two bits, then three signed 9 bit magnitudes."""
    comp = comp.astype(np.uint64)
    largest = (comp >> 30).astype(int)
    q = np.zeros((len(comp), 4))
    for i in (3, 2, 1, 0):
        here = largest != i
        mag = (comp & 511).astype(np.float64)
        neg = ((comp >> 9) & 1).astype(bool)
        value = math.sqrt(0.5) * mag / 511.0 * np.where(neg, -1.0, 1.0)
        q[here, i] = value[here]
        comp = np.where(here, comp >> 10, comp)
    rows = np.arange(len(comp))
    q[rows, largest] = np.sqrt(np.maximum(0.0, 1.0 - (q ** 2).sum(1)))
    return q


def read_splat(path):
    r = np.frombuffer(Path(path).read_bytes(), dtype=[('p', '<f4', 3), ('s', '<f4', 3), ('c', 'u1', 4), ('r', 'u1', 4)])
    return tidy(r['p'], r['c'][:, :3] / 255.0, r['c'][:, 3] / 255.0, r['s'], (r['r'] - 128.0) / 128.0)


def read(path):
    readers = {'.ply': read_ply, '.spz': read_spz, '.splat': read_splat}
    suffix = Path(path).suffix.lower()
    if suffix not in readers:
        raise SystemExit(f'{suffix} is not a splat file this reads (.ply, .spz, .splat).')
    return readers[suffix](path)


def tidy(xyz, rgb, alpha, scale, rot):
    rot = np.asarray(rot, np.float64)
    rot = rot / np.maximum(np.linalg.norm(rot, axis=1, keepdims=True), 1e-9)
    s = {'xyz': np.asarray(xyz, np.float32), 'rgb': np.clip(np.asarray(rgb, np.float32), 0, 1),
         'alpha': np.clip(np.asarray(alpha, np.float32), 0, 1), 'scale': np.asarray(scale, np.float32),
         'rot': rot.astype(np.float32)}
    good = np.isfinite(s['xyz']).all(1) & np.isfinite(s['scale']).all(1) & np.isfinite(s['rot']).all(1)
    return subset(s, good)


def subset(s, keep):
    return {k: v[keep] for k, v in s.items()}


# ---------------------------------------------------------------- the headset's frame

def quat_mul(a, b):
    aw, ax, ay, az = np.moveaxis(a, -1, 0)
    bw, bx, by, bz = np.moveaxis(b, -1, 0)
    return np.stack([aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw], axis=-1)


def find_floor(xyz):
    """The floor under you, for a file that does not say: the lowest well-populated
    two centimetre slab within three metres around, below your eyes."""
    near = (np.hypot(xyz[:, 0], xyz[:, 2]) < 3.0) & (xyz[:, 1] < -0.3)
    y = xyz[near, 1]
    if len(y) < 500:
        return float(np.percentile(xyz[:, 1], 2))
    counts, edges = np.histogram(y, bins=np.arange(y.min(), y.max() + 0.02, 0.02))
    dense = np.nonzero(counts >= 0.25 * counts.max())[0]
    return float(edges[dense[0]] + 0.01)


def to_headset(s, frame, metric_scale=None, ground_offset=None, eye_height=make_world.EYE_HEIGHT_M):
    """frame is 'opencv' (y down, z ahead: Marble, SHARP) or 'up' (y up, -z ahead)."""
    s = dict(s)
    if metric_scale:
        s['xyz'] = s['xyz'] * metric_scale
        s['scale'] = s['scale'] * metric_scale
    xyz = s['xyz'].astype(np.float64)
    if frame == 'opencv':
        # Marble's ground offset is in its own frame, after the metric scale
        if ground_offset is not None:
            xyz[:, 1] -= ground_offset
        # A half turn about x: y up and -z ahead, and every splat turned with it
        xyz = xyz * np.array([1.0, -1.0, -1.0])
        s['rot'] = quat_mul(np.array([0.0, 1.0, 0.0, 0.0]), s['rot']).astype(np.float32)
    floor = 0.0 if (frame == 'opencv' and ground_offset is not None) else find_floor(xyz)
    xyz[:, 1] -= floor + eye_height
    s['xyz'] = xyz.astype(np.float32)
    return s


def visibility(s):
    """How much of your view each splat can fill from where you sit: its opacity times
    the area of its two widest axes, over its distance squared."""
    d = np.maximum(np.linalg.norm(s['xyz'], axis=1), 0.5)
    widest = np.sort(s['scale'], axis=1)
    return s['alpha'] * widest[:, 2] * widest[:, 1] / (d * d)


def trim(s, budget):
    d = np.linalg.norm(s['xyz'], axis=1)
    s = subset(s, (d > NEAR_M) & (d < FAR_M) & (s['alpha'] >= 2 / 255))
    score = visibility(s)
    order = np.argsort(-score)
    if budget and len(order) > budget:
        order = order[:budget]
    return subset(s, order)


# ---------------------------------------------------------------- writing

def write_splat(s, path):
    """antimatter15 records, most visible first, which is what the headset reads."""
    n = len(s['xyz'])
    rot = s['rot'] * np.where(s['rot'][:, :1] < 0, -1.0, 1.0)
    record = np.zeros(n, dtype=[('p', '<f4', 3), ('s', '<f4', 3), ('c', 'u1', 4), ('r', 'u1', 4)])
    record['p'] = s['xyz']
    record['s'] = s['scale']
    record['c'] = np.clip(np.round(np.concatenate([s['rgb'], s['alpha'][:, None]], axis=1) * 255), 0, 255)
    record['r'] = np.clip(np.round(rot * 128 + 128), 0, 255)
    Path(path).write_bytes(record.tobytes())
    return n


def tile_picture(s, size=(1024, 512)):
    """The world as a panorama from your eyes, nearest splat per pixel: enough for a tile."""
    w, h = size
    d = np.linalg.norm(s['xyz'], axis=1)
    x, y, z = (s['xyz'] / d[:, None]).T
    col = ((np.arctan2(x, -z) / (2 * math.pi) + 0.5) * w).astype(int) % w
    row = np.clip((np.arccos(np.clip(y, -1, 1)) / math.pi * h).astype(int), 0, h - 1)
    image = np.full((h, w, 3), 8, np.uint8)
    far_first = np.argsort(-d)
    image[row[far_first], col[far_first]] = (s['rgb'][far_first] * 255).astype(np.uint8)
    return Image.fromarray(image)


def sit(s, seat, eye_height=make_world.EYE_HEIGHT_M):
    """Moves the world so you sit at seat (x, floor y, z, in headset metres as converted):
    that spot comes under you and its floor goes to eye height below you. For a mat the
    picture put in front of you, or a floor raised above the ground Marble measured."""
    s = dict(s)
    x, floor, z = seat
    s['xyz'] = (s['xyz'] - np.array([x, floor + eye_height, z], np.float32)).astype(np.float32)
    return s


def convert(source, name, frame, budget, metric_scale=None, ground_offset=None, panorama=None, seat=None):
    folder = make_world.BUILT / name
    folder.mkdir(parents=True, exist_ok=True)
    s = read(source)
    found = len(s['xyz'])
    s = to_headset(s, frame, metric_scale, ground_offset)
    if seat:
        s = sit(s, seat)
    s = trim(s, budget)
    kept = write_splat(s, folder / 'world.splat')
    picture = Image.open(panorama).convert('RGB') if panorama else tile_picture(s)
    picture.resize((1024, 512), Image.LANCZOS).save(folder / 'texture.jpg', quality=90)
    meta = {'source': str(source), 'frame': frame, 'found': found, 'kept': kept, 'budget': budget,
            'metric_scale_factor': metric_scale, 'ground_plane_offset': ground_offset, 'seat': seat,
            'eye_height_m': make_world.EYE_HEIGHT_M}
    (folder / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n')
    return folder, found, kept


def choose(prompt, options, default=1):
    for i, label in enumerate(options, 1):
        print(f'  {i}. {label}')
    answer = make_world.ask(prompt, str(default))
    return int(answer) - 1 if answer.isdigit() and 1 <= int(answer) <= len(options) else default - 1


def main():
    print('Turn a splat file into a headset world.\n')
    source = Path(make_world.ask('Splat file (.ply, .spz or .splat)')).expanduser()
    if not source.is_file():
        raise SystemExit('No file there.')
    # marble_world.py leaves what Marble said about the world beside its files
    sidecar = source.with_name('marble.json')
    marble = json.loads(sidecar.read_text()) if sidecar.is_file() else {}
    name = make_world.ask('Name for the world', marble.get('name') or source.stem.split('.')[0])
    print('Which way up is it?')
    frame = ['opencv', 'up'][choose('Choose', ['OpenCV, y down (Marble, SHARP, most trainers)', 'Already y up (made here, Spark files)'],
                                    1 if marble or source.suffix.lower() != '.splat' else 2)]
    print('How many splats on the headset?')
    budget = BUDGETS[choose('Choose', [label for label, _ in BUDGETS])][1]
    folder, found, kept = convert(source, name, frame, budget, marble.get('metric_scale_factor'),
                                  marble.get('ground_plane_offset'), marble.get('panorama'))
    print(f'\n{folder}')
    print(f'  {kept:,} of {found:,} splats, {(folder / "world.splat").stat().st_size / 1e6:.0f} MB')
    print('  Put it on the headset with arxvr.py, option "Put a world on the headset".')


if __name__ == '__main__':
    sys.exit(main())
