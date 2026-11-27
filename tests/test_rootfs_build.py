#!/usr/bin/env python3
"""Integration test for `qandroid rootfs build`.

Builds a real (tiny) ext4 image with mke2fs, populates it with debugfs, then
runs the builder against it.  This is the test that would have caught the
system-as-root bug: the Waydroid image has /bin -> /system/bin and a real
/system directory, so it must be extracted at the rootfs root, not nested.

Requires e2fsprogs (mke2fs + debugfs). Skips cleanly if they are absent.

  python3 tests/test_rootfs_build.py -v
"""
import importlib.machinery
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

spec = importlib.util.spec_from_loader(
    "qandroid_cli", importlib.machinery.SourceFileLoader("qandroid_cli",
                                                         os.path.join(ROOT, "qandroid")))
qandroid = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qandroid)

HAVE_E2FS = bool(shutil.which("mke2fs") and shutil.which("debugfs"))


def make_ext4(path, script_lines, size_mb=8):
    """Create an ext4 image and apply a debugfs command script to it."""
    subprocess.run(["mke2fs", "-q", "-t", "ext4", "-F", "-b", "1024",
                    path, str(size_mb * 1024)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    script = os.path.join(os.path.dirname(path), "debugfs.cmds")
    with open(script, "w") as f:
        f.write("\n".join(script_lines) + "\n")
    r = subprocess.run(["debugfs", "-w", "-f", script, path],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return path


class GuestResolveTest(unittest.TestCase):
    """Resolution of guest-absolute symlinks without a guest."""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.root, "system/bin"))
        os.makedirs(os.path.join(self.root, "system/etc"))
        open(os.path.join(self.root, "system/build.prop"), "w").close()
        os.symlink("/system/bin", os.path.join(self.root, "bin"))
        os.symlink("/system/etc", os.path.join(self.root, "etc"))
        open(os.path.join(self.root, "system/bin/sh"), "w").close()
        os.symlink("sh", os.path.join(self.root, "system/bin/sh_rel"))   # relative
        os.symlink("system/bin/sh", os.path.join(self.root, "sh_top"))   # relative, from /

    def test_absolute_symlink_resolves_inside_rootfs(self):
        got = qandroid.guest_resolve(self.root, "/bin")
        self.assertEqual(os.path.realpath(got), os.path.realpath(os.path.join(self.root, "system/bin")))

    def test_file_through_symlink(self):
        got = qandroid.guest_resolve(self.root, "/etc/../build.prop")
        self.assertIsNotNone(got)
        self.assertTrue(os.path.exists(got))

    def test_missing_path_is_none(self):
        self.assertIsNone(qandroid.guest_resolve(self.root, "/nope/nothing"))

    def test_relative_symlink(self):
        got = qandroid.guest_resolve(self.root, "/system/bin/sh_rel")
        self.assertIsNotNone(got)
        self.assertTrue(os.path.exists(got))
        # relative to the rootfs root, not to the process cwd
        got = qandroid.guest_resolve(self.root, "/sh_top")
        self.assertIsNotNone(got)
        self.assertTrue(os.path.exists(got))

    def test_symlink_loop_is_bounded(self):
        os.symlink("/loop_b", os.path.join(self.root, "loop_a"))
        os.symlink("/loop_a", os.path.join(self.root, "loop_b"))
        self.assertIsNone(qandroid.guest_resolve(self.root, "/loop_a"))


@unittest.skipUnless(HAVE_E2FS, "e2fsprogs not installed")
class RootfsBuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.imgs = os.path.join(self.tmp, "images")
        os.makedirs(self.imgs)
        self.out = os.path.join(self.tmp, "rootfs")
        self.qhome = os.path.join(self.tmp, "qhome")
        os.makedirs(self.qhome)

    def _payload(self, name, data=b"#!/bin/sh\n"):
        p = os.path.join(self.tmp, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def _build(self, image):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "qandroid"),
                            "rootfs", "build", "--image-dir", self.imgs, "--out", self.out],
                           capture_output=True, text=True,
                           env=dict(os.environ, QANDROID_HOME=self.qhome))
        return r

    def test_system_as_root_image_extracts_at_rootfs_root(self):
        """Waydroid/LineageOS 20 layout: real /system + /bin -> /system/bin."""
        img = os.path.join(self.imgs, "system.img")
        make_ext4(img, [
            "mkdir /system",
            "mkdir /system/bin",
            "mkdir /system/etc",
            "write %s /system/bin/init" % self._payload("init"),
            "symlink /system/bin/linker64 /apex/com.android.runtime/bin/linker64",
            "write %s /system/bin/linker64" % self._payload("linker"),
            "write %s /system/build.prop" % self._payload("build.prop", b"ro.build.version.sdk=34\n"),
            "symlink /bin /system/bin",
            "symlink /etc /system/etc",
        ])
        r = self._build(img)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("system-as-root", r.stdout)
        # image's own links survive; nothing was nested under rootfs/system/system
        self.assertTrue(os.path.islink(os.path.join(self.out, "bin")))
        self.assertEqual(os.readlink(os.path.join(self.out, "bin")), "/system/bin")
        self.assertTrue(os.path.exists(os.path.join(self.out, "system/bin/init")))
        self.assertFalse(os.path.exists(os.path.join(self.out, "system/system")))
        self.assertTrue(os.path.exists(os.path.join(self.out, ".qandroid-rootfs")))
        self.assertTrue(os.path.isdir(os.path.join(self.out, "data/local/tmp")))
        # init resolved; the APEX-backed linker counts as present, not missing
        self.assertIn("APEX, mounted at boot", r.stdout)
        self.assertNotRegex(r.stdout, r"/system/bin/(init|linker64)\s+MISSING")

    def test_legacy_system_partition_image_is_nested(self):
        """Android 9 style: the image is a bare /system partition."""
        img = os.path.join(self.imgs, "system.img")
        make_ext4(img, [
            "mkdir /bin",
            "mkdir /etc",
            "write %s /bin/init" % self._payload("init"),
            "write %s /build.prop" % self._payload("build.prop"),
        ])
        r = self._build(img)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("system partition", r.stdout)
        self.assertTrue(os.path.exists(os.path.join(self.out, "system/bin/init")))
        self.assertTrue(os.path.islink(os.path.join(self.out, "bin")))
        self.assertEqual(os.readlink(os.path.join(self.out, "bin")), "system/bin")
        # the guest view of /bin must land on the extracted init
        hit = qandroid.guest_resolve(self.out, "/bin/init")
        self.assertIsNotNone(hit)
        self.assertTrue(os.path.exists(hit))

    def test_missing_markers_are_reported(self):
        img = os.path.join(self.imgs, "system.img")
        make_ext4(img, ["mkdir /system", "mkdir /system/bin"])
        r = self._build(img)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("MISSING", r.stdout)
        self.assertIn("warning:", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
