#!/usr/bin/env python3
"""apk.py - inspect an APK offline: ABIs, native libs, package id, SDK levels.

Pure Python: parses the zip central directory, decodes the binary
AndroidManifest.xml (AXML) chunk format, and maps native libraries to ABIs.

  ./apk.py info app.apk [--json]
  ./apk.py libs app.apk --abi arm64-v8a -o out/     # extract .so files
  ./apk.py abis app.apk                            # just the ABI list
"""
import argparse
import json
import os
import struct
import sys
import zipfile

AXML_MAGIC = 0x0003
RES_STRING_POOL = 0x0001
RES_XML = 0x0002
RES_XML_START_ELEMENT = 0x0102
RES_XML_RESOURCE_MAP = 0x0180

ATTR_PACKAGE = 0x0101021C
ATTR_VERSION_CODE = 0x0101021B
ATTR_VERSION_NAME = 0x0101021C  # noqa: F841 (kept for reference)
ATTR_MIN_SDK = 0x0101020C
ATTR_TARGET_SDK = 0x01010270
ATTR_NAME = 0x01010003
ATTR_LABEL = 0x01010001
ATTR_DEBUGGABLE = 0x0101000F

TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11
TYPE_BOOLEAN = 0x12


class AXML:
    """Minimal res-chunk parser: string pool + start elements with attributes."""

    def __init__(self, data):
        self.data = data
        if struct.unpack_from("<H", data, 0)[0] != AXML_MAGIC:
            raise ValueError("not an AXML document")
        self.strings = []
        self.res_ids = []
        self.elements = []  # (depth, tag, {attr_name: value})
        self._parse(struct.unpack_from("<H", data, 2)[0])

    def _parse(self, base):
        d = self.data
        off = 8
        while off + 8 <= len(d):
            ctype, hsize, csize = struct.unpack_from("<HHI", d, off)
            if csize < 8 or off + csize > len(d):
                break
            body = off + hsize
            if ctype == RES_STRING_POOL:
                # offsets are relative to the chunk start, not to the string data
                self.strings = self._string_pool(off)
            elif ctype == RES_XML_RESOURCE_MAP:
                n = (off + csize - body) // 4
                self.res_ids = list(struct.unpack_from("<%dI" % n, d, body))
            elif ctype == RES_XML_START_ELEMENT:
                self._start_element(body)
            off += csize

    def _string_pool(self, chunk_start):
        d = self.data
        count, _styles, flags, strings_start, _styles_start = struct.unpack_from("<IIIII", d, chunk_start + 8)
        utf8 = flags & 0x100
        offsets = struct.unpack_from("<%dI" % count, d, chunk_start + 28)
        base = chunk_start + strings_start
        out = []
        for o in offsets:
            p = base + o
            if utf8:
                # utf16 length then utf8 length, each 1 or 2 bytes
                n = d[p]
                p += 2 if n & 0x80 else 1
                n = d[p]
                if n & 0x80:
                    n = ((n & 0x7F) << 8) | d[p + 1]
                    p += 2
                else:
                    p += 1
                out.append(d[p:p + n].decode("utf-8", "replace"))
            else:
                n = struct.unpack_from("<H", d, p)[0]
                p += 2
                if n & 0x8000:
                    n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", d, p)[0]
                    p += 2
                out.append(d[p:p + n * 2].decode("utf-16-le", "replace"))
        return out

    def string_at(self, idx):
        return self.strings[idx] if 0 <= idx < len(self.strings) else None

    def _start_element(self, at):
        d = self.data
        _ns, name_idx, attr_start, attr_size, attr_count = struct.unpack_from("<IIHHH", d, at)
        tag = self.string_at(name_idx) or ""
        attrs = {}
        for i in range(attr_count):
            p = at + attr_start + i * attr_size
            _ans, aname, raw, _size, _res0, dtype, value = struct.unpack_from("<IIIHBBI", d, p)
            name = self.string_at(aname) or ""
            if dtype == TYPE_STRING:
                val = self.string_at(raw if raw != 0xFFFFFFFF else value)
            elif dtype in (TYPE_INT_DEC, TYPE_INT_HEX):
                val = value
            elif dtype == TYPE_BOOLEAN:
                val = bool(value)
            else:
                val = value
            attrs[name] = val
        self.elements.append((0, tag, attrs))

    def manifest_attrs(self):
        for _d, tag, attrs in self.elements:
            if tag == "manifest":
                return attrs
        return {}

    def find(self, tag):
        for _d, t, attrs in self.elements:
            if t == tag:
                return attrs
        return {}


def abi_of_lib(name):
    # lib/<abi>/foo.so
    parts = name.split("/")
    if len(parts) == 3 and parts[0] == "lib":
        return parts[1]
    return None


def inspect(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        libs = {}
        for n in names:
            a = abi_of_lib(n)
            if a and n.endswith(".so"):
                libs.setdefault(a, []).append({"path": n, "size": z.getinfo(n).file_size})
        try:
            ax = AXML(z.read("AndroidManifest.xml"))
            manifest = ax.manifest_attrs()
            sdk = ax.find("uses-sdk")
            perms = sorted({a.get("name") for _d, t, a in ax.elements if t == "uses-permission"}
                           - {None})
        except (KeyError, ValueError, struct.error) as e:
            manifest, sdk, perms = {"_error": str(e)}, {}, []
        return {
            "path": path,
            "size": os.path.getsize(path),
            "package": manifest.get("package"),
            "versionCode": manifest.get("versionCode"),
            "versionName": manifest.get("versionName"),
            "minSdk": sdk.get("minSdkVersion"),
            "targetSdk": sdk.get("targetSdkVersion"),
            "debuggable": manifest.get("debuggable"),
            "abis": sorted(libs),
            "libs": libs,
            "permissions": perms,
            "entries": len(names),
        }


def main(argv):
    ap = argparse.ArgumentParser(prog="apk.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("info", "abis", "libs"):
        p = sub.add_parser(name)
        p.add_argument("apk")
        p.add_argument("--json", action="store_true")
        p.add_argument("--abi")
        p.add_argument("-o", "--out")
    a = ap.parse_args(argv)
    info = inspect(a.apk)

    if a.cmd == "info":
        if a.json:
            print(json.dumps(info, indent=2))
        else:
            print("package      %s" % info["package"])
            print("version      %s (%s)" % (info["versionName"], info["versionCode"]))
            print("sdk          min=%s target=%s" % (info["minSdk"], info["targetSdk"]))
            print("abis         %s" % (", ".join(info["abis"]) or "none (pure java/dex)"))
            for abi in info["abis"]:
                print("  %s: %d libs, %d bytes" % (abi, len(info["libs"][abi]),
                                                   sum(l["size"] for l in info["libs"][abi])))
        return 0

    if a.cmd == "abis":
        print("\n".join(info["abis"]))
        return 0

    abi = a.abi or "arm64-v8a"
    if abi not in info["libs"]:
        print("error: APK has no %s libs (have: %s)" % (abi, ", ".join(info["abis"]) or "none"), file=sys.stderr)
        return 1
    out = a.out or "."
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(a.apk) as z:
        for entry in info["libs"][abi]:
            dest = os.path.join(out, os.path.basename(entry["path"]))
            with z.open(entry["path"]) as src, open(dest, "wb") as dst:
                dst.write(src.read())
            os.chmod(dest, 0o755)
            print(dest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
