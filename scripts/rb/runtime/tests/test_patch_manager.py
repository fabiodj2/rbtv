import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'patch-manager.py'


class PatchSafety(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'rbp.stock'
        self.source.write_bytes(b'0123456789')
        self.manifest = self.root / 'manifest.json'
        self.spec = {'source_sha1': hashlib.sha1(self.source.read_bytes()).hexdigest(),
                     'patches': [{'offset': 2, 'before': '3233', 'after': 'aabb'}]}
        self.out = self.root / 'rbp.staged'

    def run_stage(self):
        self.manifest.write_text(json.dumps(self.spec))
        return subprocess.run([sys.executable, str(TOOL), 'stage', '--source', str(self.source),
                               '--manifest', str(self.manifest), '--output', str(self.out)],
                              capture_output=True)

    def test_stage_preserves_source(self):
        self.assertEqual(self.run_stage().returncode, 0)
        self.assertEqual(self.source.read_bytes(), b'0123456789')
        self.assertEqual(self.out.read_bytes(), b'01\xaa\xbb456789')

    def test_wrong_hash_refused(self):
        self.spec['source_sha1'] = '0' * 40
        self.assertNotEqual(self.run_stage().returncode, 0)
        self.assertFalse(self.out.exists())

    def test_wrong_guard_refused(self):
        self.spec['patches'][0]['before'] = 'ffff'
        self.assertNotEqual(self.run_stage().returncode, 0)
        self.assertFalse(self.out.exists())

    def test_overlap_refused(self):
        self.spec['patches'].append({'offset': 3, 'before': '33', 'after': '00'})
        self.assertNotEqual(self.run_stage().returncode, 0)
        self.assertFalse(self.out.exists())

    def test_existing_output_refused(self):
        self.out.write_bytes(b'keep')
        self.assertNotEqual(self.run_stage().returncode, 0)
        self.assertEqual(self.out.read_bytes(), b'keep')

    def test_restore(self):
        result = subprocess.run([sys.executable, str(TOOL), 'restore', '--source', str(self.source),
                                 '--output', str(self.out)], capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.out.read_bytes(), self.source.read_bytes())


if __name__ == '__main__':
    unittest.main()
