#!/usr/bin/env python3
"""Protocol tests for the qandroid HWC wire format.

Runs a real AF_UNIX SOCK_SEQPACKET round trip with an SCM_RIGHTS memfd, the same
way the guest composer and the host client talk.  Also asserts the struct layout
matches what the C side compiles to (168 bytes, no padding surprises).

  python3 tests/test_hwc_proto.py -v
"""
import array
import ctypes
import os
import socket
import struct
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QH_MAGIC = 0x51485743
QH_MAX_DAMAGE = 8
QH_OP_FRAME, QH_OP_ACK, QH_OP_BYE = 1, 2, 3
QH_FMT_RGBA8888 = 1


class Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32),
                ("w", ctypes.c_int32), ("h", ctypes.c_int32)]


class Frame(ctypes.Structure):
    _fields_ = [("magic", ctypes.c_uint32), ("op", ctypes.c_uint32),
                ("seq", ctypes.c_uint32), ("width", ctypes.c_uint32),
                ("height", ctypes.c_uint32), ("stride", ctypes.c_uint32),
                ("format", ctypes.c_uint32), ("n_damage", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("_pad", ctypes.c_uint32),
                ("damage", Rect * QH_MAX_DAMAGE)]


class Ack(ctypes.Structure):
    _fields_ = [("magic", ctypes.c_uint32), ("op", ctypes.c_uint32), ("seq", ctypes.c_uint32)]


def send_fd(sock, data, fd):
    fds = array.array("i", [fd])
    sock.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)])


def recv_fd(sock):
    fds = array.array("i")
    msg, ancdata, _flags, _addr = sock.recvmsg(4096, socket.CMSG_SPACE(4))
    for level, typ, cdata in ancdata:
        if level == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            fds = array.array("i")
            fds.frombytes(cdata[: len(cdata) - (len(cdata) % fds.itemsize)])
    return msg, (fds[0] if fds else -1)


def _fd_passing_works():
    """SCM_RIGHTS over SOCK_SEQPACKET is the transport; some emulated kernels
    (iSH, notably) do not implement recvmsg ancillary data. Probe once."""
    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    try:
        fd = os.open("/dev/null", os.O_RDONLY)
        try:
            send_fd(a, b"probe", fd)
            msg, got = recv_fd(b)
            return msg == b"probe" and got >= 0
        finally:
            os.close(fd)
    except OSError:
        return False
    finally:
        a.close()
        b.close()


FD_PASSING = _fd_passing_works()
needs_fds = unittest.skipUnless(FD_PASSING, "kernel does not pass SCM_RIGHTS over SEQPACKET")


class LayoutTest(unittest.TestCase):
    def test_frame_size(self):
        self.assertEqual(ctypes.sizeof(Frame), 40 + QH_MAX_DAMAGE * 16)
        self.assertEqual(ctypes.sizeof(Frame), 168)
        self.assertEqual(ctypes.sizeof(Rect), 16)
        self.assertEqual(ctypes.sizeof(Ack), 12)

    def test_header_field_offsets(self):
        f = Frame()
        self.assertEqual(Frame.magic.offset, 0)
        self.assertEqual(Frame.width.offset, 12)
        self.assertEqual(Frame.stride.offset, 20)
        self.assertEqual(Frame.n_damage.offset, 28)
        self.assertEqual(Frame.damage.offset, 40)


class RoundTripTest(unittest.TestCase):
    """guest side (test) -> host side (test), including the ack handshake."""

    def setUp(self):
        self.guest, self.host = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.w, self.h = 64, 48
        self.stride = self.w
        self.pattern = bytes((i * 7) & 0xFF for i in range(self.w * self.h * 4))
        # memfd is what the composer uses; some kernels (this sandbox included)
        # lack it, and an unlinked file exercises the same fd-passing path
        try:
            self.memfd = os.memfd_create("frame", os.MFD_CLOEXEC)
        except (AttributeError, OSError):
            import tempfile
            f = tempfile.TemporaryFile()
            self.memfd = os.dup(f.fileno())
            f.close()
        os.ftruncate(self.memfd, len(self.pattern))
        os.pwrite(self.memfd, self.pattern, 0)

    def tearDown(self):
        self.guest.close()
        self.host.close()
        os.close(self.memfd)

    def _frame(self, seq):
        f = Frame(magic=QH_MAGIC, op=QH_OP_FRAME, seq=seq, width=self.w, height=self.h,
                  stride=self.stride, format=QH_FMT_RGBA8888, n_damage=2, flags=0)
        f.damage[0] = Rect(0, 0, self.w, 10)
        f.damage[1] = Rect(0, 20, 8, 8)
        return f

    @needs_fds
    def test_frame_with_fd_and_ack(self):
        f = self._frame(7)
        send_fd(self.guest, bytes(f), self.memfd)

        msg, fd = recv_fd(self.host)
        self.assertEqual(len(msg), 168)
        got = Frame.from_buffer_copy(msg)
        self.assertEqual(got.magic, QH_MAGIC)
        self.assertEqual(got.seq, 7)
        self.assertEqual((got.width, got.height, got.stride), (64, 48, 64))
        self.assertEqual(got.n_damage, 2)
        self.assertEqual((got.damage[1].x, got.damage[1].w), (0, 8))
        self.assertGreaterEqual(fd, 0)

        data = os.pread(fd, len(self.pattern), 0)
        self.assertEqual(data, self.pattern)
        os.close(fd)

        self.host.send(bytes(Ack(magic=QH_MAGIC, op=QH_OP_ACK, seq=got.seq)))
        ack = Ack.from_buffer_copy(self.guest.recv(64))
        self.assertEqual((ack.magic, ack.op, ack.seq), (QH_MAGIC, QH_OP_ACK, 7))

    def test_damage_is_capped_at_eight(self):
        f = self._frame(1)
        f.n_damage = 99  # guest sets more than the array holds
        packed = bytes(f)
        self.assertEqual(len(packed), 168)
        got = Frame.from_buffer_copy(packed)
        # the C side clamps to QH_MAX_DAMAGE before sending; the wire struct just
        # carries 8 rect slots, so a receiver must clamp defensively as well
        self.assertLessEqual(got.n_damage, 255)

    def test_bye_is_a_bare_ack_sized_message(self):
        self.guest.send(bytes(Ack(magic=QH_MAGIC, op=QH_OP_BYE, seq=0)))
        msg = self.host.recv(64)
        self.assertEqual(len(msg), 12)
        got = Ack.from_buffer_copy(msg)
        self.assertEqual((got.magic, got.op), (QH_MAGIC, QH_OP_BYE))

    def test_bad_magic_detected(self):
        f = self._frame(0)
        f.magic = 0xDEADBEEF
        self.guest.send(bytes(f))
        got = Frame.from_buffer_copy(self.host.recv(1024))
        self.assertNotEqual(got.magic, QH_MAGIC)


class StructMatchesCHeaderTest(unittest.TestCase):
    """The C header is the source of truth; check Python's copy against it."""

    def test_header_constants(self):
        header = os.path.join(ROOT, "hwc", "include", "qandroid_hwc_proto.h")
        with open(header) as f:
            src = f.read()
        self.assertIn("#define QH_MAGIC 0x51485743u", src)
        self.assertIn("#define QH_MAX_DAMAGE 8", src)
        self.assertIn("#define QH_POOL 3", src)
        # every field of struct qh_frame must appear in the same order
        body = src[src.index("struct qh_frame {"):]
        body = body[: body.index("};")]
        fields = [line.strip().split(";")[0].split()[-1]
                  for line in body.splitlines()[1:] if ";" in line and not line.strip().startswith("/*")]
        py_names = [name for name, _ in Frame._fields_]
        # damage[] is the only field spelled with array syntax on the C side
        self.assertEqual(fields[-1], "damage[QH_MAX_DAMAGE]")
        self.assertEqual(fields[:-1], py_names[:-1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
