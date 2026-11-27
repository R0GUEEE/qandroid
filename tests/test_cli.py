#!/usr/bin/env python3
"""Offline tests for the parts of the qandroid CLI that do not need a guest:
the SourceForge RSS parser and the qemu patcher (against a fake qemu tree).

  python3 tests/test_cli.py -v
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

spec = importlib.util.spec_from_loader(
    "qandroid_cli",
    importlib.machinery.SourceFileLoader("qandroid_cli", os.path.join(ROOT, "qandroid")))
qandroid = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qandroid)

RSS_FIXTURE = """<?xml version="1.0"?>
<rss version="2.0">
  <channel>
    <item>
      <title><![CDATA[/images/system/lineage/waydroid_arm64/lineage-20.0-20260927-VANILLA-waydroid_arm64-system.zip]]></title>
      <link>https://sourceforge.net/projects/waydroid/files/images/system/lineage/waydroid_arm64/lineage-20.0-20260927-VANILLA-waydroid_arm64-system.zip/download</link>
      <media:content type="application/zip" filesize="912267974"><media:hash algo="md5">cfb47ba9e88be4d1a1b3f9d2b3e3b5aa</media:hash></media:content>
    </item>
    <item>
      <title><![CDATA[/images/system/lineage/waydroid_x86_64/lineage-20.0-20260927-GAPPS-waydroid_x86_64-system.zip]]></title>
      <link>https://example.invalid/x86_64.zip/download</link>
      <media:content type="application/zip" filesize="1500000000"><media:hash algo="md5">00112233445566778899001122334455</media:hash></media:content>
    </item>
    <item>
      <title>broken item with no link</title>
    </item>
  </channel>
</rss>
"""


class RssTest(unittest.TestCase):
    def test_parses_entries_and_skips_broken(self):
        items = qandroid.parse_rss(RSS_FIXTURE)
        self.assertEqual(len(items), 2)
        first = items[0]
        self.assertEqual(first["name"],
                         "lineage-20.0-20260927-VANILLA-waydroid_arm64-system.zip")
        self.assertEqual(first["size"], 912267974)
        self.assertEqual(first["md5"], "cfb47ba9e88be4d1a1b3f9d2b3e3b5aa")
        self.assertTrue(first["url"].endswith("/download"))

    def test_abi_filtering_matches_only_the_arm64_dir(self):
        items = qandroid.parse_rss(RSS_FIXTURE)
        arm = [i for i in items if "waydroid_arm64/" in i["path"]]
        self.assertEqual(len(arm), 1)
        self.assertNotIn("x86_64", arm[0]["path"])


FAKE_QEMU_IOCTL = """\
static abi_long do_ioctl(CPUArchState *cpu_env, int fd, abi_long cmd, abi_long arg)
{
    return -TARGET_ENOTTY;
}

"""

FAKE_QEMU_SYSCALL = """\
#include <linux/prctl.h>

static abi_long do_prctl(CPUArchState *env, abi_long option, abi_long arg2,
                         abi_long arg3, abi_long arg4, abi_long arg5)
{
    switch (option) {
    case PR_SET_NAME:
        return 0;
    default:
        return -TARGET_EINVAL;
    }
}
"""


class PatchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.tmp, "linux-user"))
        self.src = os.path.join(self.tmp, "linux-user", "syscall.c")
        with open(self.src, "w") as f:
            f.write(FAKE_QEMU_SYSCALL + FAKE_QEMU_IOCTL)
        self.apply = os.path.join(ROOT, "patches", "apply.sh")

    def test_patch_is_applied_once_and_idempotent(self):
        r = subprocess.run(["python3", self.apply, self.tmp], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.src) as f:
            text = f.read()
        self.assertIn("QANDROID_PR_SET_VMA", text)
        self.assertEqual(text.count("option == 0x53564d41"), 1)
        # guard sits inside the function body, before the switch
        body = text[text.index("do_prctl("):]
        self.assertLess(body.index("0x53564d41"), body.index("switch (option)"))

        r2 = subprocess.run(["python3", self.apply, self.tmp], capture_output=True, text=True)
        self.assertEqual(r2.returncode, 0)
        self.assertIn("already patched", r2.stdout)
        with open(self.src) as f:
            self.assertEqual(f.read().count("option == 0x53564d41"), 1)

    def test_missing_anchor_fails_loudly(self):
        with open(self.src, "w") as f:
            f.write("int main(void) { return 0; }\n")
        r = subprocess.run(["python3", self.apply, self.tmp], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("anchor not found", r.stderr)

    def test_binder_ioctl_passthrough_is_added(self):
        r = subprocess.run(["python3", self.apply, self.tmp], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.src) as f:
            text = f.read()
        self.assertIn("QANDROID_BINDER_IOCTL", text)
        self.assertIn("== 'b'", text)
        # the guard must sit inside do_ioctl, before its body
        body = text[text.index("do_ioctl("):]
        self.assertLess(body.index("QANDROID_BINDER_IOCTL"), body.index("TARGET_ENOTTY"))
        # and the prctl patch must still be there
        self.assertIn("QANDROID_PR_SET_VMA", text)

    def test_not_a_qemu_tree(self):
        r = subprocess.run(["python3", self.apply, tempfile.mkdtemp()],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("does not look like a qemu tree", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
