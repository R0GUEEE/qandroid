#!/usr/bin/env python3
"""lpunpack.py - pure-Python Android dynamic partition (super.img) unpacker.

No AOSP build tools required. Implements LP_METADATA_* (Android 10+ logical
partitions) well enough to list and extract every LINEAR extent to plain files.

  ./lpunpack.py list    super.img
  ./lpunpack.py extract super.img outdir [--partition system_a ...]
  ./lpunpack.py info    super.img        # geometry + block devices + groups

Metadata layout (see system/core/fs_mgr/liblp/include/liblp/metadata_format.h):

  offset 4096              LpMetadataGeometry   (magic 0x616c4467), 4096 bytes
  8192 + metadata_max_size*slot   LpMetadataHeader (magic 0x414c5030)
                                        [header][partitions][extents][groups][block_devices]

All table descriptors hold offsets relative to the start of the header.
"""
import argparse
import json
import os
import struct
import sys
import zlib

GEOMETRY_MAGIC = 0x616C4467
HEADER_MAGIC = 0x414C5030
GEOMETRY_OFFSET = 4096
LP_METADATA_GEOMETRY_SIZE = 4096
METADATA_OFFSET = GEOMETRY_OFFSET + LP_METADATA_GEOMETRY_SIZE
SECTOR = 512

TARGET_LINEAR = 0


def crc32(data, seed=0):
    return zlib.crc32(data, seed) & 0xFFFFFFFF


class LPError(Exception):
    pass


class Table:
    __slots__ = ("offset", "num_entries", "entry_size")

    def __init__(self, buf, at):
        self.offset, self.num_entries, self.entry_size = struct.unpack_from("<III", buf, at)


class Geometry:
    def __init__(self, buf):
        (magic, self.struct_size, self.checksum, self.metadata_max_size,
         self.metadata_slot_count, self.logical_block_size) = struct.unpack_from("<6I", buf, 0)
        if magic != GEOMETRY_MAGIC:
            raise LPError("bad geometry magic 0x%08x at %d" % (magic, GEOMETRY_OFFSET))
        if self.logical_block_size % SECTOR or not self.logical_block_size:
            raise LPError("implausible logical_block_size %d" % self.logical_block_size)
        if not (1 <= self.metadata_slot_count <= 4):
            raise LPError("implausible metadata_slot_count %d" % self.metadata_slot_count)


class Partition:
    def __init__(self, name, attributes, first_extent, num_extents, group_index):
        self.name = name
        self.attributes = attributes
        self.first_extent = first_extent
        self.num_extents = num_extents
        self.group_index = group_index

    def __repr__(self):
        return "<Partition %s extents=%d+%d>" % (self.name, self.first_extent, self.num_extents)


class Extent:
    __slots__ = ("num_sectors", "target_type", "target_data", "target_source")

    def __init__(self, buf, at):
        self.num_sectors, self.target_type, self.target_data, self.target_source = struct.unpack_from("<QIII", buf, at)


class BlockDevice:
    def __init__(self, buf, at):
        (self.first_logical_sector, self.alignment, self.alignment_offset,
         self.size, name, self.flags) = struct.unpack_from("<QIIQ36sI", buf, at)
        self.name = name.split(b"\0", 1)[0].decode("utf-8", "replace")


class Group:
    def __init__(self, buf, at):
        name, self.flags, self.maximum_size = struct.unpack_from("<36sIQ", buf, at)
        self.name = name.split(b"\0", 1)[0].decode("utf-8", "replace")


class Super:
    def __init__(self, path, metadata_slot=0):
        self.path = path
        self.metadata_slot = metadata_slot
        self.size = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(GEOMETRY_OFFSET)
            self.geo = Geometry(f.read(64))
            header_at = METADATA_OFFSET + self.metadata_slot * self.geo.metadata_max_size
            f.seek(header_at)
            head = f.read(4096)
        (magic, major, minor, header_size, header_crc, tables_size, tables_crc) = struct.unpack_from("<IHHIIII", head, 0)
        if magic != HEADER_MAGIC:
            raise LPError("bad metadata header magic 0x%08x at %d" % (magic, header_at))
        self.version = "%d.%d" % (major, minor)
        self.header_size = header_size
        self.header_ok = (crc32(head[:12] + b"\0\0\0\0" + head[16:header_size]) == header_crc)
        self.tables_ok = (crc32(head[header_size:header_size + tables_size]) == tables_crc)
        self.partitions_t = Table(head, 24)
        self.extents_t = Table(head, 36)
        self.groups_t = Table(head, 48)
        self.block_devices_t = Table(head, 60)
        self.flags = struct.unpack_from("<I", head, 72)[0] if header_size >= 76 else 0
        self.tables = head  # descriptors reference offsets inside this buffer

        self.block_devices = [BlockDevice(head, t.offset + i * t.entry_size)
                              for t in [self.block_devices_t] for i in range(t.num_entries)]
        self.groups = [Group(head, self.groups_t.offset + i * self.groups_t.entry_size)
                       for i in range(self.groups_t.num_entries)]
        self.extents = [Extent(head, self.extents_t.offset + i * self.extents_t.entry_size)
                        for i in range(self.extents_t.num_entries)]
        self.partitions = []
        pt = self.partitions_t
        for i in range(pt.num_entries):
            at = pt.offset + i * pt.entry_size
            name, attrs, first, num, grp = struct.unpack_from("<36sIIII", head, at)
            self.partitions.append(Partition(name.split(b"\0", 1)[0].decode("utf-8", "replace"),
                                             attrs, first, num, grp))

    # ---- reporting -----------------------------------------------------
    def info(self):
        return {
            "path": self.path,
            "size": self.size,
            "version": self.version,
            "logical_block_size": self.geo.logical_block_size,
            "metadata_max_size": self.geo.metadata_max_size,
            "metadata_slot_count": self.geo.metadata_slot_count,
            "header_checksum_ok": self.header_ok,
            "tables_checksum_ok": self.tables_ok,
            "flags": self.flags,
            "block_devices": [
                {"name": b.name, "first_logical_sector": b.first_logical_sector,
                 "size": b.size, "alignment": b.alignment} for b in self.block_devices],
            "groups": [{"name": g.name, "maximum_size": g.maximum_size} for g in self.groups],
            "partitions": [
                {"name": p.name, "attributes": p.attributes, "num_extents": p.num_extents,
                 "group": (self.groups[p.group_index].name if p.group_index < len(self.groups) else None),
                 "size": self.partition_size(p)}
                for p in self.partitions],
        }

    def partition_size(self, part):
        total = 0
        for i in range(part.num_extents):
            e = self.extents[part.first_extent + i]
            if e.target_type == TARGET_LINEAR:
                total += e.num_sectors * SECTOR
        return total

    # ---- extraction ----------------------------------------------------
    def extents_for(self, part):
        """Yield (file_offset_in_super, length) for every LINEAR extent."""
        for i in range(part.num_extents):
            e = self.extents[part.first_extent + i]
            if e.target_type != TARGET_LINEAR:
                raise LPError("partition %s: unsupported extent target_type %d (only LINEAR)"
                              % (part.name, e.target_type))
            src = self.block_devices[e.target_source] if e.target_source < len(self.block_devices) else None
            base = src.first_logical_sector if src else 0
            yield (base + e.target_data) * SECTOR, e.num_sectors * SECTOR

    def extract(self, part, out_path, sparse=None, progress=True):
        total = self.partition_size(part)
        done = 0
        with open(self.path, "rb") as src, open(out_path, "wb") as dst:
            for off, length in self.extents_for(part):
                if off + length > self.size:
                    raise LPError("%s: extent [%d,%d) past end of image (%d)"
                                  % (part.name, off, off + length, self.size))
                src.seek(off)
                remaining = length
                while remaining:
                    chunk = src.read(min(8 << 20, remaining))
                    if not chunk:
                        raise LPError("%s: short read" % part.name)
                    dst.write(chunk)
                    remaining -= len(chunk)
                    done += len(chunk)
                    if progress and total:
                        sys.stderr.write("\r  %-28s %5.1f%%" % (part.name, 100.0 * done / total))
                if progress:
                    sys.stderr.write("\r  %-28s 100.0%%\n" % part.name)
        return total


def main(argv):
    ap = argparse.ArgumentParser(prog="lpunpack.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("list", "info", "extract"):
        p = sub.add_parser(name)
        p.add_argument("image")
        p.add_argument("outdir", nargs="?")
        p.add_argument("--partition", action="append", default=[])
        p.add_argument("--json", action="store_true")
        p.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    try:
        sup = Super(a.image)
    except LPError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    if a.cmd == "info":
        print(json.dumps(sup.info(), indent=2))
        return 0

    if a.cmd == "list":
        if a.json:
            print(json.dumps([{"name": p.name, "size": sup.partition_size(p)} for p in sup.partitions], indent=2))
        else:
            print("%-24s %12s  %s" % ("PARTITION", "SIZE", "GROUP"))
            for p in sup.partitions:
                print("%-24s %12d  %s" % (p.name, sup.partition_size(p), sup.groups[p.group_index].name))
        return 0

    if not a.outdir:
        ap.error("extract needs an output directory")
    if not (sup.header_ok and sup.tables_ok):
        print("warning: metadata checksum mismatch (header=%s tables=%s) - extracting anyway"
              % (sup.header_ok, sup.tables_ok), file=sys.stderr)
    os.makedirs(a.outdir, exist_ok=True)
    wanted = set(a.partition)
    todo = [p for p in sup.partitions if not wanted or p.name in wanted]
    if wanted - {p.name for p in todo}:
        print("error: unknown partition(s): %s" % ", ".join(sorted(wanted - {p.name for p in todo})), file=sys.stderr)
        return 2
    for p in todo:
        out = os.path.join(a.outdir, p.name + ".img")
        n = sup.extract(p, out, progress=not a.quiet)
        print("%s  %d bytes" % (out, n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
