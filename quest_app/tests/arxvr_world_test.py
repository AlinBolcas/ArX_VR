"""The world generator against the rules the headset's loader actually enforces.

Every check here mirrors a refusal in xr_room.c: wrong magic, counts that make no
sense, a file whose size disagrees with its header, an index past the vertices. Plus
the two things that look wrong in the headset rather than failing: a split seam and a
spiked pole.
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host_tools/worlds'))
import make_world

# From xr_renderer.h
ROOM_MAX_VERTS = 65535
ROOM_MODEL_FLOATS = 8


def build_world(depth_image):
    depth = make_world.load_depth(depth_image, make_world.SEGMENTS + 1, make_world.RINGS + 1)
    position, uv = make_world.build(0.1 + depth, 'depth')
    return position, make_world.normals(position), uv


def gradient_depth(path):
    rows = np.linspace(0, 255, make_world.RINGS + 1, dtype=np.uint8)
    Image.fromarray(np.tile(rows[:, None], (1, make_world.SEGMENTS + 1))).save(path)
    return path


class WorldFileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        depth = gradient_depth(root / 'depth.png')
        picture = root / 'picture.png'
        Image.new('RGB', (512, 256), (40, 60, 90)).save(picture)
        cls.position, cls.normal, cls.uv = build_world(depth)
        cls.folder = root / 'world'
        make_world.write_world(cls.folder, cls.position, cls.normal, cls.uv, Image.open(picture))
        cls.data = (cls.folder / 'model.room').read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def header(self):
        return struct.unpack('<II', self.data[4:12])

    def test_the_loader_recognises_it(self):
        self.assertEqual(self.data[:4], b'MXR1')

    def test_counts_are_within_what_the_loader_takes(self):
        vertices, indices = self.header()
        self.assertGreater(vertices, 0)
        self.assertLessEqual(vertices, ROOM_MAX_VERTS)
        self.assertEqual(indices % 3, 0)

    def test_size_matches_the_header(self):
        vertices, indices = self.header()
        self.assertEqual(len(self.data), 12 + vertices * ROOM_MODEL_FLOATS * 4 + indices * 2)

    def test_no_index_points_past_the_vertices(self):
        vertices, indices = self.header()
        at = 12 + vertices * ROOM_MODEL_FLOATS * 4
        drawn = np.frombuffer(self.data[at:at + indices * 2], dtype='<u2')
        self.assertLess(int(drawn.max()), vertices)
        # Every vertex is used, or the grid is not what it claims to be
        self.assertEqual(len(np.unique(drawn)), vertices)

    def test_the_texture_ships_beside_the_model(self):
        self.assertTrue((self.folder / 'texture.png').is_file())

    def test_the_seam_down_the_back_meets_itself(self):
        left, right = self.position[:, 0], self.position[:, -1]
        self.assertLess(float(np.abs(np.linalg.norm(left, axis=-1) - np.linalg.norm(right, axis=-1)).max()), 1e-4)

    def test_the_poles_are_one_point(self):
        for pole in (self.position[0], self.position[-1]):
            spread = float(np.linalg.norm(pole - pole.mean(axis=0), axis=-1).max())
            self.assertLess(spread, 1e-3)

    def test_everything_sits_inside_the_limits(self):
        distance = np.linalg.norm(self.position.reshape(-1, 3), axis=-1)
        self.assertGreaterEqual(float(distance.min()), make_world.MIN_M - 0.01)
        self.assertLessEqual(float(distance.max()), make_world.MAX_M + 0.01)

    def test_the_floor_is_at_eye_height_whatever_the_model_scale(self):
        # The same world at ten times the scale must land at the same size
        _, lat = make_world.grid_angles()
        base = np.full(lat.shape, 3.0, dtype=np.float32)
        for scale in (1.0, 10.0):
            position, _ = make_world.build(base * scale, 'depth')
            straight_down = -position[-1, :, 1].mean()
            self.assertAlmostEqual(float(straight_down), make_world.EYE_HEIGHT_M, places=2)

    def test_the_floor_near_your_feet_is_flat(self):
        # Every vertex within the floor cap sits on the plane at eye height, however bumpy the guess
        _, lat = make_world.grid_angles()
        bumpy = (2.0 + np.random.default_rng(1).random(lat.shape)).astype(np.float32)
        position, _ = make_world.build(bumpy, 'depth')
        cap = np.degrees(np.pi - lat) < make_world.FLOOR_PLANE_DEG
        heights = position[..., 1][cap]
        self.assertLess(float(np.abs(heights + make_world.EYE_HEIGHT_M).max()), 0.02)

    def test_normals_face_the_viewer(self):
        facing = np.sum(self.normal.reshape(-1, 3) * self.position.reshape(-1, 3), axis=-1)
        self.assertLessEqual(float(facing.max()), 0.0)

    def test_texture_coordinates_cover_the_whole_picture(self):
        self.assertAlmostEqual(float(self.uv[..., 0].min()), 0.0, places=5)
        self.assertAlmostEqual(float(self.uv[..., 0].max()), 1.0, places=5)
        self.assertAlmostEqual(float(self.uv[..., 1].min()), 0.0, places=5)
        self.assertAlmostEqual(float(self.uv[..., 1].max()), 1.0, places=5)

    def test_disparity_white_is_near_and_distance_grows_with_its_value(self):
        # The one mistake that turns a world inside out, and the first version made it
        white, black = np.float32(1.0), np.float32(0.0)
        self.assertLess(float(make_world.distances(white, 'disparity')), float(make_world.distances(black, 'disparity')))
        self.assertLess(float(make_world.distances(np.float32(2.0), 'depth')), float(make_world.distances(np.float32(9.0), 'depth')))

    def test_disparity_spends_its_range_on_what_is_close(self):
        # Half way in disparity is much nearer than half way in distance, as inverse depth is
        middle = float(make_world.distances(np.float32(0.5), 'disparity'))
        self.assertLess(middle, (make_world.DISPARITY_NEAR + make_world.DISPARITY_FAR) / 2)

    def test_measured_metres_are_kept_and_the_floor_left_alone(self):
        # A Blender cast is the truth: nothing is rescaled and no plane is put under it
        grid = np.full((make_world.RINGS + 1, make_world.SEGMENTS + 1), 4.0, dtype=np.float32)
        grid[-1, :] = 1.6
        position, _ = make_world.build(grid, 'metres')
        distance = np.linalg.norm(position, axis=-1)
        self.assertAlmostEqual(float(distance[make_world.RINGS // 2, 10]), 4.0, places=3)
        self.assertAlmostEqual(float(distance[-1].mean()), 1.6, places=3)

    def test_a_blender_depth_file_loads_at_the_grid_size(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'depth_m.npy'
            np.save(path, np.full((360, 720), 7.5, dtype=np.float32))
            grid = make_world.load_metres(path, make_world.SEGMENTS + 1, make_world.RINGS + 1)
            self.assertEqual(grid.shape, (make_world.RINGS + 1, make_world.SEGMENTS + 1))
            self.assertAlmostEqual(float(grid.mean()), 7.5, places=3)

    def test_a_flat_depth_map_is_refused_rather_than_making_a_ball(self):
        with tempfile.TemporaryDirectory() as temp:
            flat = Path(temp) / 'flat.png'
            Image.new('L', (64, 32), 128).save(flat)
            with self.assertRaises(SystemExit):
                make_world.load_depth(flat, 64, 32)


# ---------------------------------------------------------------- splat worlds

import gzip
import math
import subprocess

import splat_convert

APP = Path(__file__).resolve().parents[1]
JNI = APP / 'streaming/src/main/jni/xr-renderer'


def covariance(scale, rot):
    """Sigma = R S S R^T, w x y z quaternions, the way the headset builds it."""
    w, x, y, z = rot.T
    r = np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                  2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                  2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)], axis=1).reshape(-1, 3, 3)
    m = r * scale[:, None, :]
    return m @ m.transpose(0, 2, 1)


def random_splats(n, seed=5):
    rng = np.random.default_rng(seed)
    q = rng.normal(size=(n, 4))
    return {'xyz': rng.uniform(-4, 4, (n, 3)).astype(np.float32), 'rgb': rng.uniform(0, 1, (n, 3)),
            'alpha': rng.uniform(0.2, 1, n), 'scale': np.exp(rng.uniform(-6, -1, (n, 3))),
            'rot': q / np.linalg.norm(q, axis=1, keepdims=True)}


def write_ply(path, s):
    """The 3DGS layout Marble exports, logits and logs and all."""
    n = len(s['xyz'])
    names = ['x', 'y', 'z', 'f_dc_0', 'f_dc_1', 'f_dc_2', 'opacity', 'scale_0', 'scale_1', 'scale_2',
             'rot_0', 'rot_1', 'rot_2', 'rot_3']
    columns = np.concatenate([s['xyz'], (s['rgb'] - 0.5) / splat_convert.SH_C0,
                              np.log(s['alpha'] / (1 - s['alpha']))[:, None], np.log(s['scale']), s['rot']], axis=1)
    head = 'ply\nformat binary_little_endian 1.0\nelement vertex %d\n' % n
    head += ''.join(f'property float {k}\n' for k in names) + 'end_header\n'
    Path(path).write_bytes(head.encode() + columns.astype('<f4').tobytes())


def write_spz(path, s, version):
    """Packed the way Niantic's packGaussians does it, then gzipped: v2 and v3."""
    n, fractional = len(s['xyz']), 12
    fixed = np.round(s['xyz'] * (1 << fractional)).astype(np.int64) & 0xFFFFFF
    positions = np.stack([fixed & 255, (fixed >> 8) & 255, (fixed >> 16) & 255], axis=-1).astype(np.uint8)
    alpha = np.round(s['alpha'] * 255).astype(np.uint8)
    dc = (s['rgb'] - 0.5) / splat_convert.SH_C0
    colors = np.clip(np.round(dc * (0.15 * 255) + 127.5), 0, 255).astype(np.uint8)
    scales = np.clip(np.round((np.log(s['scale']) + 10) * 16), 0, 255).astype(np.uint8)
    q = s['rot'][:, [1, 2, 3, 0]]
    if version >= 3:
        rotations = np.zeros(n, np.uint32)
        for k in range(n):
            big = int(np.argmax(np.abs(q[k])))
            negate = q[k, big] < 0
            comp = big
            for i in range(4):
                if i != big:
                    neg = int((q[k, i] < 0) ^ negate)
                    comp = (comp << 10) | (neg << 9) | int(511 * abs(q[k, i]) / math.sqrt(0.5) + 0.5)
            rotations[k] = comp
        rotations = rotations.astype('<u4').tobytes()
    else:
        signed = q * np.where(q[:, 3:] < 0, -127.5, 127.5) + 127.5
        rotations = np.clip(np.round(signed[:, :3]), 0, 255).astype(np.uint8).tobytes()
    header = struct.pack('<IIIBBBB', splat_convert.SPZ_MAGIC, version, n, 0, fractional, 0, 0)
    body = header + positions.tobytes() + alpha.tobytes() + colors.tobytes() + scales.tobytes() + rotations
    Path(path).write_bytes(gzip.compress(body))


class SplatWorldTest(unittest.TestCase):
    def test_ply_reads_back_what_was_written(self):
        s = random_splats(300)
        with tempfile.TemporaryDirectory() as temp:
            write_ply(Path(temp) / 'a.ply', s)
            back = splat_convert.read(Path(temp) / 'a.ply')
        np.testing.assert_allclose(back['xyz'], s['xyz'], atol=1e-6)
        np.testing.assert_allclose(back['alpha'], s['alpha'], atol=1e-5)
        np.testing.assert_allclose(covariance(back['scale'], back['rot']), covariance(s['scale'], s['rot']), atol=1e-6)

    def test_spz_v2_and_v3_read_back_within_their_quantisation(self):
        s = random_splats(400)
        for version in (2, 3):
            with tempfile.TemporaryDirectory() as temp:
                write_spz(Path(temp) / 'a.spz', s, version)
                back = splat_convert.read(Path(temp) / 'a.spz')
            np.testing.assert_allclose(back['xyz'], s['xyz'], atol=1 / 4096)
            np.testing.assert_allclose(back['rgb'], s['rgb'], atol=0.02)
            np.testing.assert_allclose(back['alpha'], s['alpha'], atol=1 / 255)
            np.testing.assert_allclose(np.log(back['scale']), np.log(s['scale']), atol=1 / 32 + 1e-6)
            # Same rotation up to sign, to the precision each version keeps
            agree = np.abs((back['rot'] * s['rot']).sum(1))
            self.assertGreater(agree.min(), 0.995 if version >= 3 else 0.97, version)

    def test_marble_frame_puts_the_floor_under_you_and_turns_every_splat(self):
        # Marble's raw frame is OpenCV: y down, so a floor 1.6 m below the camera is at +1.6
        rng = np.random.default_rng(1)
        floor = {'xyz': np.stack([rng.uniform(-3, 3, 4000), np.full(4000, 1.6), rng.uniform(-3, 3, 4000)], 1),
                 'rgb': np.full((4000, 3), 0.5), 'alpha': np.full(4000, 0.9), 'scale': np.full((4000, 3), 0.02),
                 'rot': np.tile([1.0, 0, 0, 0], (4000, 1))}
        # One long splat pointing ahead (OpenCV +z) and one turned oddly
        extra = random_splats(2, seed=9)
        extra['scale'][0] = [0.01, 0.01, 0.5]
        extra['rot'][0] = [1, 0, 0, 0]
        s = splat_convert.tidy(*[np.concatenate([floor[k], extra[k]]) for k in ('xyz', 'rgb', 'alpha', 'scale', 'rot')])
        out = splat_convert.to_headset(s, 'opencv', metric_scale=1.0, ground_offset=1.6)
        self.assertAlmostEqual(float(np.median(out['xyz'][:4000, 1])), -make_world.EYE_HEIGHT_M, places=4)
        flip = np.diag([1.0, -1.0, -1.0])
        want = flip @ covariance(s['scale'][4000:], s['rot'][4000:]) @ flip
        np.testing.assert_allclose(covariance(out['scale'][4000:], out['rot'][4000:]), want, atol=1e-6)
        # And the long one still points along z, which is ahead in both frames
        self.assertGreater(abs(covariance(out['scale'][4000:4001], out['rot'][4000:4001])[0, 2, 2]), 0.2)
        # Without Marble's numbers the floor is found instead
        found = splat_convert.to_headset(s, 'opencv')
        self.assertAlmostEqual(float(np.median(found['xyz'][:4000, 1])), -make_world.EYE_HEIGHT_M, delta=0.02)

    def test_the_budget_keeps_what_you_can_see(self):
        # A thousand small specks, and ten big splats two metres ahead
        s = random_splats(1000)
        s['scale'] *= 0.1
        s['xyz'][:10] = [[0, 0, -2.0]] * 10
        s['scale'][:10] = 0.3
        s['alpha'][:10] = 1.0
        kept = splat_convert.trim(s, 100)
        self.assertEqual(len(kept['xyz']), 100)
        self.assertTrue(np.allclose(kept['xyz'][:10], [0, 0, -2.0]))

    def test_the_headset_code_packs_a_converted_world(self):
        # The headset's own C, built here: its checks, then the file this tool wrote
        s = random_splats(3000)
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            binary = temp / 'splat_data_test'
            subprocess.run(['clang', '-O2', '-Wall', '-Werror', '-o', str(binary), str(APP / 'tests/splat_data_test.c'),
                            str(JNI / 'xr_splat_data.c'), '-lm'], check=True)
            checks = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(checks.returncode, 0, checks.stdout)
            write_ply(temp / 'w.ply', s)
            original = make_world.BUILT
            make_world.BUILT = temp
            try:
                folder, found, kept = splat_convert.convert(temp / 'w.ply', 'w', 'opencv', 2000, 1.0, 1.6)
            finally:
                make_world.BUILT = original
            self.assertEqual((found, kept), (3000, 2000))
            self.assertEqual((folder / 'world.splat').stat().st_size, 2000 * 32)
            with Image.open(folder / 'texture.jpg') as tile:
                self.assertEqual(tile.size, (1024, 512))
            packed = subprocess.run([str(binary), 'pack', str(folder / 'world.splat'), str(temp / 't.bin')],
                                    capture_output=True, text=True, check=True)
            self.assertTrue(packed.stdout.startswith('2000 splats'), packed.stdout)

    def test_the_test_page_reads_the_shaders_the_headset_compiles(self):
        import splat_harness
        vertex, fragment = splat_harness.shader_sources()
        self.assertTrue(vertex.startswith('#version 300 es\n') and fragment.startswith('#version 300 es\n'))
        self.assertIn('gl_VertexID', vertex)
        self.assertNotIn('//', vertex)


class SplatMergeTest(unittest.TestCase):
    def test_far_specks_in_one_cell_become_one_splat_and_near_ones_stay(self):
        import splat_merge
        far = np.array([[0.0, 0.0, -10.0], [0.001, 0.0, -10.0], [0.0, 0.001, -10.0]])
        near = np.array([[0.2, 0.0, -1.0], [0.201, 0.0, -1.0]])
        n = 5
        s = splat_convert.tidy(np.concatenate([far, near]), np.full((n, 3), 0.5), np.full(n, 0.6),
                               np.full((n, 3), 0.002), np.tile([1.0, 0, 0, 0], (n, 1)))
        out, gone = splat_merge.merge(s, cell_deg=0.4)
        self.assertEqual(gone, 2)
        self.assertEqual(len(out['xyz']), 3)
        merged = out['xyz'][-1]
        np.testing.assert_allclose(merged, far.mean(axis=0), atol=1e-6)
        self.assertLessEqual(float(out['alpha'].max()), 1.0)
        # Its spread covers where the three were
        self.assertGreater(float(out['scale'][-1].max()), 0.002)
        # The near pair is untouched
        np.testing.assert_allclose(out['xyz'][:2], near, atol=1e-6)

    def test_merged_rotation_rebuilds_the_merged_covariance(self):
        import splat_merge
        rng = np.random.default_rng(3)
        q = rng.normal(size=(50, 4))
        q /= np.linalg.norm(q, axis=1, keepdims=True)
        m = splat_merge.quat_to_matrix(q)
        np.testing.assert_allclose(splat_merge.quat_to_matrix(splat_merge.matrix_to_quat(m)), m, atol=1e-9)


if __name__ == '__main__':
    unittest.main()
