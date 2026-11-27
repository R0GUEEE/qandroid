#!/usr/bin/env python3
"""apply.sh - patch a qemu source tree for Android userspace compatibility.

    ./patches/apply.sh /path/to/qemu-src

Only one edit is actually required; the rest of the Android ABI happens to work
under linux-user already (see patches/README.md for the audit).  The edit is
done by anchor rather than by line number so it survives small version drift,
and it is idempotent.
"""
import os
import sys

GUARD = "QANDROID_PR_SET_VMA"

SNIPPET = """
    /* %s
     * bionic names anonymous mappings with
     *   prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, addr, len, name)
     * (scudo / jemalloc / art heaps do this constantly).  The name is purely
     * cosmetic for debuggers; linux-user guest addresses are host addresses,
     * so the name would even land on the right mapping - but qemu has no
     * mapping registry here.  Answer success and drop it.
     */
    if (option == 0x53564d41 /* PR_SET_VMA */) {
        return 0;
    }
""" % GUARD

DEFINE = """
#ifndef PR_SET_VMA
# define PR_SET_VMA 0x53564d41  /* %s */
#endif
""" % GUARD


def patch(path, anchor, insert_after_open_brace=True):
    with open(path) as f:
        src = f.read()
    if GUARD in src:
        print("already patched: %s" % path)
        return False
    at = src.find(anchor)
    if at < 0:
        sys.exit("anchor not found in %s: %r (qemu layout changed - inspect do_prctl)"
                 % (path, anchor))
    brace = src.index("{", at)
    src = src[:brace + 1] + SNIPPET + src[brace + 1:]
    # macro near the top, after the last include block is fine too; put it first
    src = DEFINE + src
    with open(path, "w") as f:
        f.write(src)
    print("patched %s at %s" % (path, anchor))
    return True


def main(argv):
    if len(argv) != 1:
        sys.exit(__doc__)
    root = argv[0]
    target = os.path.join(root, "linux-user", "syscall.c")
    if not os.path.exists(target):
        sys.exit("%s does not look like a qemu tree (no linux-user/syscall.c)" % root)
    changed = patch(target, "static abi_long do_prctl(")
    # keep the tree tidy: qemu builds with -Werror in some CI setups
    if changed:
        print("\nnow rebuild:  cd %s && ./configure --target-list=aarch64-linux-user "
              "--static --disable-system && make -j$(nproc)" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
