import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "inspect-rbp.py"


class InspectRbp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.binary = root / "rbp"
        self.binary.write_bytes(b"0123456789")
        self.manifest = root / "manifest.json"

    def write_manifest(self, approved=True, expected="3233"):
        digest = hashlib.sha1(self.binary.read_bytes()).hexdigest()
        self.manifest.write_text(json.dumps({
            "source_sha1": digest if approved else "",
            "patches": [{"offset": 2, "before": expected, "after": "aabb", "label": "test"}],
        }))

    def run_tool(self):
        return subprocess.run([
            sys.executable, str(TOOL), "--rbp", str(self.binary),
            "--manifest", str(self.manifest), "--json",
        ], capture_output=True, text=True)

    def test_approved_binary_passes(self):
        self.write_manifest()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0)
        report = json.loads(result.stdout)
        self.assertTrue(report["sha1_approved"])
        self.assertEqual(report["guards_matching"], 1)

    def test_candidate_manifest_remains_blocked(self):
        self.write_manifest(approved=False)
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(result.stdout)["sha1_approved"])

    def test_guard_mismatch_remains_blocked(self):
        self.write_manifest(expected="ffff")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["guards_matching"], 0)


if __name__ == "__main__":
    unittest.main()
