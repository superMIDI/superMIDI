"""Builds and runs tests/supermidi_host.c with the computer's C compiler (skipped without one).

The test includes patches/supermidi.c and replaces the stock firmware's functions and RAM
with stand-ins, so it checks the patch's logic: profile bits, entry overrides, the editing
protocol, saving and loading with a fake flash page. It cannot check the hooks themselves.
Run: python -m unittest discover -s tests
"""
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no host C compiler")
class SuperMidiHost(unittest.TestCase):
    def test_logic(self):
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "t.exe")
            subprocess.run([CC, "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-Wundef",
                            "-I", HERE, "-I", os.path.join(HERE, "..", "patches"),
                            "-o", exe, os.path.join(HERE, "supermidi_host.c")], check=True)
            out = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout)
            self.assertIn("all passed", out.stdout)


if __name__ == "__main__":
    unittest.main()
