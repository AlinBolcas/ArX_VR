"""Verify native screenshot orientation and rejection of incomplete captures."""
from pathlib import Path
import importlib.util
import json
import tempfile
import unittest
from PIL import Image

spec = importlib.util.spec_from_file_location('arxvr_device_lab', Path(__file__).resolve().parent / 'arxvr_device_lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)


class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'cap_user_01_source.json'
        self.metadata = {'width': 1, 'height': 2, 'format': 'RGBA', 'origin': 'bottom-left'}
        self.path.write_text(json.dumps(self.metadata))
        self.path.with_suffix('.raw').write_bytes(bytes([255, 0, 0, 255, 0, 0, 255, 255]))

    def test_bottom_left_pixels_become_upright_png(self):
        with Image.open(lab.decode_desktop_capture(self.path)) as frame:
            self.assertEqual(list(frame.getdata()), [(0, 0, 255, 255), (255, 0, 0, 255)])

    def test_partial_transfer_is_not_presented_as_a_screenshot(self):
        self.path.with_suffix('.raw').write_bytes(b'partial')
        with self.assertRaises(ValueError): lab.decode_desktop_capture(self.path)
        self.assertFalse(self.path.with_suffix('.png').exists())

    def test_rejects_invalid_dimensions_and_format(self):
        for key, value in [('width', 0), ('height', 8193), ('width', 1.5), ('format', 'RGB'), ('origin', 'unknown')]:
            metadata = dict(self.metadata); metadata[key] = value
            self.path.write_text(json.dumps(metadata))
            with self.assertRaises(ValueError): lab.decode_desktop_capture(self.path)


if __name__ == '__main__': unittest.main()
