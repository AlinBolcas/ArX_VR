"""Fewer splats, the same picture from where you sit: clusters merged offline, no retraining.

You sit on the mat, so every splat has a distance from your eyes, and the headset can only
resolve so fine an angle. A cluster of splats that together cover less than that angle is
one blob to the eye anyway, so it becomes one splat that carries the same light:

    weight     each splat's opacity times its visible area
    centre     the weighted mean of the centres
    shape      the weighted mean of each covariance plus how the centres spread around the
               new centre (moment matching: the merged Gaussian has the cluster's own extent)
    colour     the weighted mean
    opacity    the cluster's summed coverage over the merged splat's area, at most 1

Clusters are cells of a grid that grows with distance, a fixed angle wide at every range, so
near things keep every splat and the far wall loses the ones it never needed. Splats already
bigger than their cell are left as they are.
"""
from pathlib import Path
import math
import sys

import numpy as np

import splat_convert

# A Quest 2 shows about 20 pixels a degree; a cell of about 3 pixels is far below what a
# seated viewer can tell apart once the splats in it are blended
CELL_DEG = 0.15
# Nothing within this distance is touched: it is what you lean towards
KEEP_NEAR_M = 1.5


def quat_to_matrix(q):
    w, x, y, z = q.T
    return np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                     2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                     2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)], axis=1).reshape(-1, 3, 3)


def matrix_to_quat(m):
    """Rotation matrices to w x y z quaternions, stable for every rotation."""
    t = m[:, 0, 0] + m[:, 1, 1] + m[:, 2, 2]
    q = np.zeros((len(m), 4))
    cases = [t > 0, (m[:, 0, 0] >= m[:, 1, 1]) & (m[:, 0, 0] >= m[:, 2, 2]), m[:, 1, 1] >= m[:, 2, 2], np.ones(len(m), bool)]
    done = np.zeros(len(m), bool)
    for k, case in enumerate(cases):
        sel = case & ~done
        done |= sel
        a = m[sel]
        if k == 0:
            s = np.sqrt(t[sel] + 1.0) * 2
            q[sel] = np.stack([0.25 * s, (a[:, 2, 1] - a[:, 1, 2]) / s, (a[:, 0, 2] - a[:, 2, 0]) / s, (a[:, 1, 0] - a[:, 0, 1]) / s], 1)
        elif k == 1:
            s = np.sqrt(1.0 + a[:, 0, 0] - a[:, 1, 1] - a[:, 2, 2]) * 2
            q[sel] = np.stack([(a[:, 2, 1] - a[:, 1, 2]) / s, 0.25 * s, (a[:, 0, 1] + a[:, 1, 0]) / s, (a[:, 0, 2] + a[:, 2, 0]) / s], 1)
        elif k == 2:
            s = np.sqrt(1.0 + a[:, 1, 1] - a[:, 0, 0] - a[:, 2, 2]) * 2
            q[sel] = np.stack([(a[:, 0, 2] - a[:, 2, 0]) / s, (a[:, 0, 1] + a[:, 1, 0]) / s, 0.25 * s, (a[:, 1, 2] + a[:, 2, 1]) / s], 1)
        else:
            s = np.sqrt(1.0 + a[:, 2, 2] - a[:, 0, 0] - a[:, 1, 1]) * 2
            q[sel] = np.stack([(a[:, 1, 0] - a[:, 0, 1]) / s, (a[:, 0, 2] + a[:, 2, 0]) / s, (a[:, 1, 2] + a[:, 2, 1]) / s, 0.25 * s], 1)
    return q / np.linalg.norm(q, axis=1, keepdims=True)


def merge(s, cell_deg=CELL_DEG, keep_near=KEEP_NEAR_M):
    """The splats with every under-resolved cluster merged into one; returns it and the count merged away."""
    xyz = s['xyz'].astype(np.float64)
    d = np.linalg.norm(xyz, axis=1)
    # Cells a fixed angle wide, stepped in distance shells so a cell has one size
    angle = math.radians(cell_deg)
    shell = np.floor(np.log(np.maximum(d, keep_near) / keep_near) / math.log(1.25)).astype(np.int64)
    cell = keep_near * 1.25 ** shell * angle
    big = s['scale'].max(axis=1) >= cell
    mergeable = (d > keep_near) & ~big

    idx = np.nonzero(mergeable)[0]
    grid = np.floor(xyz[idx] / cell[idx, None]).astype(np.int64)
    key = np.stack([shell[idx], grid[:, 0], grid[:, 1], grid[:, 2]], axis=1)
    _, group, sizes = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    group = group.ravel()
    many = sizes[group] > 1
    idx, group = idx[many], group[many]
    # Groups numbered 0..k-1 again, only the ones that actually merge
    _, group = np.unique(group, return_inverse=True)
    k = int(group.max()) + 1 if len(group) else 0
    if k == 0:
        return s, 0

    sc = s['scale'][idx].astype(np.float64)
    wide = np.sort(sc, axis=1)
    area = wide[:, 2] * wide[:, 1]
    weight = s['alpha'][idx].astype(np.float64) * area + 1e-12
    total = np.bincount(group, weight, k)

    def mean(values):
        return np.stack([np.bincount(group, weight * values[:, j], k) for j in range(values.shape[1])], 1) / total[:, None]

    centre = mean(xyz[idx])
    colour = mean(s['rgb'][idx].astype(np.float64))
    r = quat_to_matrix(s['rot'][idx].astype(np.float64))
    cov = (r * sc[:, None, :] ** 2) @ r.transpose(0, 2, 1)
    off = xyz[idx] - centre[group]
    cov = cov + off[:, :, None] * off[:, None, :]
    merged_cov = np.stack([np.bincount(group, weight * cov[:, a, b], k) for a in range(3) for b in range(3)], 1).reshape(k, 3, 3) / total[:, None, None]
    values, vectors = np.linalg.eigh(merged_cov)
    values = np.maximum(values, 1e-10)
    vectors[np.linalg.det(vectors) < 0, :, 0] *= -1
    scale = np.sqrt(values)
    merged_area = np.sort(scale, axis=1)[:, 2] * np.sort(scale, axis=1)[:, 1]
    coverage = np.bincount(group, s['alpha'][idx] * area, k)
    alpha = np.clip(coverage / merged_area, 0.0, 1.0)

    keep = np.ones(len(xyz), bool)
    keep[idx] = False
    out = {key: s[key][keep] for key in s}
    added = {'xyz': centre.astype(np.float32), 'rgb': colour.astype(np.float32), 'alpha': alpha.astype(np.float32),
             'scale': scale.astype(np.float32), 'rot': matrix_to_quat(vectors).astype(np.float32)}
    out = {key: np.concatenate([out[key], added[key]]) for key in out}
    return out, len(idx) - k


def main():
    print('Merge the splats you cannot tell apart from the mat.\n')
    folder = Path(splat_convert.make_world.ask('World folder (holding world.splat)')).expanduser()
    source = folder / 'world.splat'
    if not source.is_file():
        raise SystemExit('No world.splat there.')
    # 0.4 is what the shipped worlds used on a Quest 2; 0.6 is about a third fewer again, softer far away
    cell = float(splat_convert.make_world.ask('Merge angle in degrees, bigger means fewer splats', '0.4'))
    s = splat_convert.read(source)
    merged, gone = merge(s, cell)
    out = folder.with_name(folder.name + '_merged')
    out.mkdir(exist_ok=True)
    splat_convert.write_splat(splat_convert.trim(merged, None), out / 'world.splat')
    for name in ('texture.jpg', 'panorama.jpg', 'ambient.ogg'):
        if (folder / name).is_file():
            (out / name).write_bytes((folder / name).read_bytes())
    print(f'{len(s["xyz"]):,} -> {len(merged["xyz"]):,} splats ({gone:,} merged away)\n{out}')


if __name__ == '__main__':
    sys.exit(main())
