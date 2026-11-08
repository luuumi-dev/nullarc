import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('phx_profile', ROOT / 'tools/phx_profile.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileTests(unittest.TestCase):
    def test_capture_and_generated_header(self):
        p = profile.load(profile.PROFILE)
        self.assertEqual(p['cu_active_number'], 8)
        self.assertEqual(profile.HEADER.read_text(), profile.render(p))

    def test_malformed_topologies(self):
        original = profile.load(profile.PROFILE)
        for key, value in [('cu_active_number', 12), ('num_cu_per_sh', 0),
                           ('num_shader_engines', 5), ('num_rb_pipes', 33),
                           ('enabled_rb_pipes_mask', 16), ('format', True),
                           ('cu_active_number', 8.0), ('cu_bitmap', [[15, 15]])]:
            p = copy.deepcopy(original)
            p[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                profile.validate(p)
        for se, sa, value in [(1, 0, 1), (0, 2, 1), (0, 0, 64), (0, 0, -1)]:
            p = copy.deepcopy(original)
            p['cu_bitmap'][se][sa] = value
            with self.assertRaises(ValueError):
                profile.validate(p)

    def test_sparse_physical_masks(self):
        p = copy.deepcopy(profile.load(profile.PROFILE))
        p['cu_bitmap'][0][:2] = [0x33, 0x2d]
        self.assertIs(profile.validate(p), p)
        self.assertIn('0x33u, 0x2du', profile.render(p))

    def test_duplicate_json_key(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'bad.json'
            path.write_text('{"format": 1, "format": 1}')
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                profile.load(path)


if __name__ == '__main__':
    unittest.main()
