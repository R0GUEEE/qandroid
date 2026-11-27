#!/usr/bin/env python3
"""Unit tests for tools/lpunpack.py and tools/apk.py using synthetic inputs.

Both tools are built to be usable without any AOSP tooling, so the fixtures are
generated here byte-for-byte from the documented on-disk formats.

  python3 tests/test_tools.py -v
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import lpunpack  # noqa: E402
import apk as apktool  # noqa: E402

SECTOR = 512
GEOMETRY_AT = 4096
METADATA_MAX = 65536


# --------------------------------------------------------------------------
# synthetic super.img
# --------------------------------------------------------------------------
def build_super(path, partitions, data_area_sectors=2048, block_device_base=2048):
    """partitions: [(name, [(target_data_sectors, payload_bytes), ...])]"""
    pt, et, payload = b"", b"", []
    first_extent = 0
    for name, extents in partitions:
        for target, blob in extents:
            et += struct.pack("<QIII4x", len(blob) // SECTOR, 0, target, 0)
            payload.append((target, blob))
        pt += struct.pack("<36sIIII", name.encode(), 0, first_extent, len(extents), 0)
        first_extent += len(extents)
    gt = struct.pack("<36sIQ", b"default", 0, 0)
    bdt = struct.pack("<QIIQ36sI", block_device_base, 1 << 20, 0,
                      data_area_sectors * SECTOR, b"super", 0)

    header_size = 256
    tables = pt + et + gt + bdt
    sizes = [(header_size, len(pt)//52 if pt else 0, 52),
             (header_size + len(pt), len(et)//24 if et else 0, 24),
             (header_size + len(pt) + len(et), 1, 48),
             (header_size + len(pt) + len(et) + 48, 1, 64)]
    tables_off = [s[0] for s in sizes]

    header = struct.pack("<IHHIIII", 0x414C5030, 10, 2, header_size, 0, len(tables), 0)
    for (o, n, esz) in sizes:
        header += struct.pack("<III", o, n, esz)
    header += struct.pack("<I", 0)
    header += b"\0" * (header_size - len(header))
    # tables_checksum lives inside the hashed header range, so patch it first
    tcrc = zlib.crc32(tables) & 0xFFFFFFFF
    header = header[:20] + struct.pack("<I", tcrc) + header[24:]
    hcrc = zlib.crc32(header[:12] + b"\0\0\0\0" + header[16:header_size]) & 0xFFFFFFFF
    header = header[:12] + struct.pack("<I", hcrc) + header[16:]

    geo = struct.pack("<6I", 0x616C4467, 52, 0, METADATA_MAX, 2, 4096)
    geo += b"\0" * (52 - len(geo))
    geo += b"\0" * (4096 - len(geo))

    with open(path, "wb") as f:
        f.write(b"\0" * GEOMETRY_AT)
        f.write(geo)
        f.write(header)
        f.write(tables)
        end = GEOMETRY_AT + METADATA_MAX + METADATA_MAX  # 2 slots
        f.write(b"\0" * (end - f.tell()))
        f.truncate(max(end, (block_device_base + data_area_sectors) * SECTOR))
        for target, blob in payload:
            f.seek((block_device_base + target) * SECTOR)
            f.write(blob)
    return path


class SuperTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.payload_sys = bytes(range(256)) * 16          # 4096 bytes
        self.payload_vnd = b"V" * 2048                      # 2048 bytes
        self.img = os.path.join(self.tmp, "super.img")
        build_super(self.img, [
            ("system_a", [(0, self.payload_sys)]),
            ("vendor_a", [(8, self.payload_vnd)]),
        ])

    def test_geometry_and_tables(self):
        s = lpunpack.Super(self.img)
        self.assertEqual(s.geo.metadata_max_size, METADATA_MAX)
        self.assertEqual(s.geo.logical_block_size, 4096)
        self.assertTrue(s.header_ok, "header checksum")
        self.assertTrue(s.tables_ok, "tables checksum")
        self.assertEqual([p.name for p in s.partitions], ["system_a", "vendor_a"])
        self.assertEqual(s.partition_size(s.partitions[0]), len(self.payload_sys))
        self.assertEqual(s.groups[0].name, "default")
        self.assertEqual(s.block_devices[0].name, "super")

    def test_extract_roundtrip(self):
        out = os.path.join(self.tmp, "out")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "lpunpack.py"),
                            "extract", self.img, out, "--quiet"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(os.path.join(out, "system_a.img"), "rb") as f:
            self.assertEqual(f.read(), self.payload_sys)
        with open(os.path.join(out, "vendor_a.img"), "rb") as f:
            self.assertEqual(f.read(), self.payload_vnd)

    def test_bad_magic(self):
        bad = os.path.join(self.tmp, "bad.img")
        with open(bad, "wb") as f:
            f.write(b"\0" * (GEOMETRY_AT + 64))
        with self.assertRaises(lpunpack.LPError):
            lpunpack.Super(bad)

    def test_extent_overflow_detected(self):
        img = os.path.join(self.tmp, "short.img")
        build_super(img, [("oem_a", [(4, b"Z" * 4096)])])
        with open(img, "r+b") as f:
            f.truncate((2048 + 8) * SECTOR)  # cut the extent in half
        s = lpunpack.Super(img)
        with self.assertRaises(lpunpack.LPError):
            s.extract(s.partitions[0], os.path.join(self.tmp, "x.img"), progress=False)


# --------------------------------------------------------------------------
# synthetic apk with a real binary AndroidManifest.xml
# --------------------------------------------------------------------------
def _pool(strings):
    blobs, offsets = b"", []
    for s in strings:
        offsets.append(len(blobs))
        b = s.encode()
        blobs += bytes([len(s), len(b)]) + b + b"\0"
    while len(blobs) % 4:
        blobs += b"\0"
    start = 28 + 4 * len(strings)
    size = start + len(blobs)
    out = struct.pack("<HHIIIIII", 0x0001, 28, size, len(strings), 0, 0x100, start, 0)
    out += struct.pack("<%dI" % len(strings), *offsets)
    return out + blobs


def _start_element(name_idx, attrs, nstrings):
    """attrs: [(name_idx, type, value)]"""
    body = struct.pack("<IIHHHHHH", 0xFFFFFFFF, name_idx, 20, 20, len(attrs), 0, 0, 0)
    for (n, t, v) in attrs:
        body += struct.pack("<IIIHBBI", 0xFFFFFFFF, n, 0xFFFFFFFF, 8, 0, t, v)
    size = 16 + len(body)
    return struct.pack("<HHIII", 0x0102, 16, size, 1, 0xFFFFFFFF) + body


def build_axml(path, package="com.example.probe", version_code=42,
               version_name="1.0", min_sdk=24, target_sdk=34):
    strings = ["manifest", "package", "versionCode", "versionName",
               "uses-sdk", "minSdkVersion", "targetSdkVersion",
               "uses-permission", "name", "android.permission.INTERNET", package, version_name]
    idx = {s: i for i, s in enumerate(strings)}
    S, I, D = apktool.TYPE_STRING, apktool.TYPE_INT_DEC, apktool.TYPE_INT_DEC
    chunks = _pool(strings)
    chunks += _start_element(idx["manifest"], [
        (idx["package"], S, idx[package]),
        (idx["versionCode"], I, version_code),
        (idx["versionName"], S, idx[version_name]),
    ], len(strings))
    chunks += _start_element(idx["uses-sdk"], [
        (idx["minSdkVersion"], D, min_sdk),
        (idx["targetSdkVersion"], D, target_sdk),
    ], len(strings))
    chunks += _start_element(idx["uses-permission"], [
        (idx["name"], S, idx["android.permission.INTERNET"]),
    ], len(strings))
    total = 8 + len(chunks)
    return struct.pack("<HHI", 0x0003, 8, total) + chunks


class ApkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.apk = os.path.join(self.tmp, "probe.apk")
        axml = build_axml(self.apk)
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("AndroidManifest.xml", axml)
            z.writestr("classes.dex", b"dex\n035\0" + b"\0" * 64)
            z.writestr("lib/arm64-v8a/libprobe.so", b"\x7fELF" + b"\0" * 124)
            z.writestr("lib/armeabi-v7a/libprobe.so", b"\x7fELF" + b"\0" * 124)
            z.writestr("lib/x86_64/libprobe.so", b"\x7fELF" + b"\0" * 124)

    def test_manifest_attrs(self):
        info = apktool.inspect(self.apk)
        self.assertEqual(info["package"], "com.example.probe")
        self.assertEqual(info["versionCode"], 42)
        self.assertEqual(info["versionName"], "1.0")
        self.assertEqual(info["minSdk"], 24)
        self.assertEqual(info["targetSdk"], 34)
        self.assertEqual(info["abis"], ["arm64-v8a", "armeabi-v7a", "x86_64"])
        self.assertIn("android.permission.INTERNET", info["permissions"])

    def test_lib_extract(self):
        out = os.path.join(self.tmp, "libs")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "apk.py"),
                            "libs", self.apk, "--abi", "arm64-v8a", "-o", out],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        names = sorted(os.listdir(out))
        self.assertEqual(names, ["libprobe.so"])
        self.assertTrue(os.access(os.path.join(out, "libprobe.so"), os.X_OK))

    def test_missing_abi(self):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "apk.py"),
                            "libs", self.apk, "--abi", "riscv64", "-o", self.tmp],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("no riscv64 libs", r.stderr)

    def test_utf8_pool_roundtrip(self):
        strings = ["manifest", "päckage", "\u4f60\u597d"]
        data = _pool(strings)
        doc = struct.pack("<HHI", 0x0003, 8, 8 + len(data)) + data
        ax = apktool.AXML(doc)
        self.assertEqual(ax.strings, strings)


if __name__ == "__main__":
    unittest.main(verbosity=2)
