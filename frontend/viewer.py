#!/usr/bin/env python3
"""viewer.py - v0.1 display frontend for the qandroid guest.

No compositor plumbing: it polls the guest's `screencap` for raw RGBA frames and
blits them into an SDL window via pygame, sending touch events back through
`input tap`.  That is honest about being slow (3-10 fps); it exists so the guest
is usable before the HWC backend lands, and it needs nothing but a running
SurfaceFlinger.

  ./frontend/viewer.py --root ~/.local/share/qandroid/rootfs
  ./frontend/viewer.py --dump frame.raw       # one frame, no display
"""
import argparse
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOPDIR = os.path.dirname(HERE)
sys.path.insert(0, TOPDIR)


def guest_cmd(rootfs, argv, timeout=20):
    return subprocess.run(["chroot", rootfs] + argv, capture_output=True, timeout=timeout)


def grab_frame(rootfs):
    """screencap without -p prints w,h,format then RGBA8888 pixels."""
    r = guest_cmd(rootfs, ["/system/bin/screencap"])
    if r.returncode != 0 or len(r.stdout) < 16:
        raise RuntimeError("screencap failed: %s" % (r.stderr.decode("utf-8", "replace").strip() or
                                                    "%d bytes" % len(r.stdout)))
    w, h, fmt = struct.unpack("<III", r.stdout[:12])
    pixels = r.stdout[16:] if len(r.stdout) >= w * h * 4 + 16 else r.stdout[12:]
    if len(pixels) < w * h * 4:
        raise RuntimeError("short frame: %d bytes for %dx%d" % (len(pixels), w, h))
    return w, h, pixels[: w * h * 4]


def tap(rootfs, x, y):
    guest_cmd(rootfs, ["/system/bin/input", "tap", str(x), str(y)], timeout=10)


def swipe(rootfs, x1, y1, x2, y2, ms=200):
    guest_cmd(rootfs, ["/system/bin/input", "swipe", str(x1), str(y1), str(x2), str(y2), str(ms)],
              timeout=10)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/.local/share/qandroid/rootfs"))
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--dump", help="write one raw frame here and exit")
    ap.add_argument("--scale", type=float, default=1.0)
    a = ap.parse_args(argv)

    if not os.path.isdir(os.path.join(a.root, "system")):
        print("no rootfs at %s" % a.root, file=sys.stderr)
        return 2

    if a.dump:
        w, h, px = grab_frame(a.root)
        with open(a.dump, "wb") as f:
            f.write(struct.pack("<II", w, h) + px)
        print("%dx%d -> %s (%d bytes)" % (w, h, a.dump, len(px)))
        return 0

    try:
        import pygame  # noqa: F401
    except ImportError:
        print("pygame is not installed.  Either `pip install pygame`, or use "
              "`--dump frame.raw` and play it with\n"
              "  ffplay -f rawvideo -pixel_format rgba -video_size WxH frame.raw",
              file=sys.stderr)
        return 2

    import pygame

    pygame.init()
    screen = None
    clock = pygame.time.Clock()
    dragging = None
    print("viewer: click to tap, drag to swipe, q to quit")
    try:
        while True:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT or (ev.type == pygame.KEYDOWN and ev.key == pygame.K_q):
                    return 0
                if screen is None:
                    continue
                if ev.type == pygame.MOUSEBUTTONDOWN:
                    dragging = (ev.pos[0] / a.scale, ev.pos[1] / a.scale)
                elif ev.type == pygame.MOUSEBUTTONUP and dragging:
                    x0, y0 = dragging
                    x1, y1 = ev.pos[0] / a.scale, ev.pos[1] / a.scale
                    dragging = None
                    if abs(x1 - x0) < 8 and abs(y1 - y0) < 8:
                        tap(a.root, int(x0), int(y0))
                    else:
                        swipe(a.root, int(x0), int(y0), int(x1), int(y1))
            try:
                w, h, px = grab_frame(a.root)
            except RuntimeError as e:
                print("frame error: %s" % e, file=sys.stderr)
                time.sleep(1.0)
                continue
            if screen is None:
                sw, sh = int(w * a.scale), int(h * a.scale)
                screen = pygame.display.set_mode((sw, sh))
                pygame.display.set_caption("qandroid guest (%dx%d)" % (w, h))
            surf = pygame.image.frombuffer(px, (w, h), "RGBA")
            if a.scale != 1.0:
                surf = pygame.transform.smoothscale(surf, screen.get_size())
            screen.blit(surf, (0, 0))
            pygame.display.flip()
            clock.tick(a.fps)
    finally:
        pygame.quit()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
