#!/usr/bin/env python3
"""Tests for the Debian packaging.

Builds a real .deb with packaging/build-deb.sh (no binaries, so no compiler is
needed), then inspects the archive the way dpkg would: control fields, file
list, permissions, the /usr/bin symlink, maintainer scripts and md5sums.

Skips cleanly when dpkg-deb is absent.

  python3 tests/test_packaging.py -v
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HAVE_DPKG = bool(shutil.which("dpkg-deb"))


@unittest.skipUnless(HAVE_DPKG, "dpkg-deb not installed")
class DebTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp()
        r = subprocess.run([os.path.join(ROOT, "packaging", "build-deb.sh"),
                            "--arch", "all", "--no-binaries", "--out", cls.out],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("build-deb.sh failed:\n%s\n%s" % (r.stdout, r.stderr))
        debs = [f for f in os.listdir(cls.out) if f.endswith(".deb")]
        assert len(debs) == 1, debs
        cls.deb = os.path.join(cls.out, debs[0])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def _info(self):
        return subprocess.run(["dpkg-deb", "--field", self.deb],
                              capture_output=True, text=True).stdout

    def _contents(self):
        return subprocess.run(["dpkg-deb", "--contents", self.deb],
                              capture_output=True, text=True).stdout

    def test_control_fields(self):
        info = self._info()
        self.assertIn("Package: qandroid", info)
        self.assertIn("Architecture: all", info)
        self.assertRegex(info, r"Version: \d+\.\d+\.\d+")
        self.assertIn("Depends: python3", info)
        self.assertIn("qemu-user-static", info)
        self.assertIn("Description: run arm64 Android APKs on x86_64 Linux", info)
        for field in ("Maintainer:", "Section:", "Priority:", "Homepage:"):
            self.assertIn(field, info)

    def test_payload_files_present(self):
        listing = self._contents()
        for path in ("usr/lib/qandroid/qandroid",
                     "usr/lib/qandroid/tools/apk.py",
                     "usr/lib/qandroid/tools/lpunpack.py",
                     "usr/lib/qandroid/scripts/30-boot.sh",
                     "usr/lib/qandroid/scripts/guest-boot.sh",
                     "usr/lib/qandroid/patches/apply.sh",
                     "usr/lib/qandroid/frontend/hwc_client.c",
                     "usr/lib/qandroid/hwc/src/hwcomposer_qandroid.c",
                     "usr/share/man/man1/qandroid.1.gz",
                     "usr/share/doc/qandroid/README.md",
                     "usr/share/doc/qandroid/docs/BRINGUP.md",
                     "usr/share/doc/qandroid/copyright"):
            self.assertTrue(any(path in line for line in listing.splitlines()),
                            "missing from package: %s" % path)

    def test_no_build_artifacts_leak(self):
        listing = self._contents()
        for bad in ("__pycache__", ".pyc", "binder-mk\n", "hwc_client\n"):
            self.assertNotIn(bad, listing)
        # sources for the helpers are shipped, binaries are not
        self.assertIn("tools/binder-mk.c", listing)
        self.assertIn("tools/binder-ver.c", listing)

    def test_executable_bits(self):
        contents = self._contents()
        for line in contents.splitlines():
            if "usr/lib/qandroid/qandroid" in line or "tools/apk.py" in line:
                self.assertTrue(line.startswith("-rwxr-xr-x"), line)

    def test_symlink_and_maintainer_script(self):
        contents = self._contents()
        sym = [l for l in contents.splitlines() if "usr/bin/qandroid ->" in l]
        self.assertEqual(len(sym), 1)
        self.assertTrue(sym[0].startswith("lrwxrwxrwx"), sym[0])
        self.assertIn("../lib/qandroid/qandroid", sym[0])
        self.assertIn("postinst", subprocess.run(
            ["dpkg-deb", "--info", self.deb], capture_output=True, text=True).stdout)

    def test_extracted_tree_runs(self):
        """The installed layout must actually work, symlink and all."""
        root = tempfile.mkdtemp()
        try:
            subprocess.run(["dpkg-deb", "-x", self.deb, root], check=True)
            exe = os.path.join(root, "usr", "bin", "qandroid")
            self.assertTrue(os.path.exists(exe))
            r = subprocess.run([exe, "--version"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(r.stdout.startswith("qandroid "), r.stdout)
            # the CLI must find its own tree through the symlink
            r = subprocess.run([exe, "--help"], capture_output=True, text=True)
            self.assertIn("doctor", r.stdout)
            self.assertIn("setup", r.stdout)
        finally:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
