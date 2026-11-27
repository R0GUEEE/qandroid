#!/usr/bin/env bash
# build-deb.sh - build an installable .deb for qandroid.
#
#   ./packaging/build-deb.sh                    # native arch, with helpers built
#   ./packaging/build-deb.sh --arch all --no-binaries
#   ./packaging/build-deb.sh --version 0.3.1 --out dist/
#
# Needs only dpkg-deb (dpkg-dev). Helpers (binder-mk, binder-ver, hwc_client)
# are compiled for the build host unless --no-binaries is given, in which case
# the package ships sources and the CLI builds them on first use.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
cd "$ROOT"

VERSION=$(python3 -c "import re;print(re.search(r'^VERSION = \"([^\"]+)\"',open('qandroid').read(),re.M).group(1))")
# Debian arch name: dpkg only knows it on a Debian-derived host, otherwise map
# the machine type (this script also runs on Alpine and inside containers).
detect_arch() {
    if [ -f /etc/debian_version ] && command -v dpkg >/dev/null; then
        dpkg --print-architecture 2>/dev/null && return
    fi
    case "$(uname -m)" in
        x86_64|amd64) echo amd64 ;;
        aarch64|arm64) echo arm64 ;;
        armv7l|armv7) echo armhf ;;
        i?86) echo i386 ;;
        *) echo all ;;
    esac
}
ARCH=$(detect_arch)
OUT=$ROOT/dist
WITH_BINARIES=1
HAS_SDL=0
MAINTAINER=${MAINTAINER:-R0GUEEE <R0GUEEE@users.noreply.github.com>}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION=$2; shift 2 ;;
        --arch) ARCH=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --no-binaries) WITH_BINARIES=0; shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

command -v dpkg-deb >/dev/null || { echo "dpkg-deb is required (apt install dpkg-dev)" >&2; exit 1; }
[ "$WITH_BINARIES" = 0 ] && ARCH=all

PKG=qandroid
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
chmod 0755 "$STAGE"   # mktemp gives 0700; the package root must be world-readable
APPDIR=$STAGE/usr/lib/$PKG
DOCDIR=$STAGE/usr/share/doc/$PKG
MANDIR=$STAGE/usr/share/man/man1
mkdir -p "$APPDIR" "$DOCDIR" "$MANDIR" "$STAGE/usr/bin" "$STAGE/DEBIAN"

echo "== staging $PKG $VERSION ($ARCH) =="

# ---- payload -------------------------------------------------------------
# cp -a per tree, then prune: portable across GNU and busybox toolchains, unlike
# `install --target-directory` or find -printf.
install -m0755 qandroid "$APPDIR/qandroid"
for d in tools scripts patches frontend shim hwc; do
    [ -d "$d" ] || continue
    mkdir -p "$APPDIR/$d"
    cp -a "$d"/. "$APPDIR/$d"/
done
rm -f "$APPDIR/tools/binder-mk" "$APPDIR/tools/binder-ver" \
      "$APPDIR/frontend/hwc_client" "$APPDIR/shim/libabshim.so" \
      "$APPDIR/hwc/hwcomposer.qandroid.so" "$APPDIR/Makefile"
find "$APPDIR" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
find "$APPDIR" -name '*.pyc' -delete 2>/dev/null || true
# executables stay executables, headers and sources stay readable
chmod 0755 "$APPDIR/qandroid"
for f in tools/apk.py tools/lpunpack.py patches/apply.sh frontend/viewer.py; do
    [ -e "$APPDIR/$f" ] && chmod 0755 "$APPDIR/$f"
done
for f in "$APPDIR"/scripts/*.sh; do [ -e "$f" ] && chmod 0755 "$f"; done
chmod 0644 "$APPDIR"/hwc/include/*.h "$APPDIR"/hwc/src/*.c 2>/dev/null || true

# ---- helpers -------------------------------------------------------------
if [ "$WITH_BINARIES" = 1 ]; then
    CC=${CC:-cc}
    if command -v "$CC" >/dev/null; then
        echo "-- building host helpers with $CC"
        "$CC" -O2 -Wall -Wextra -o "$APPDIR/tools/binder-mk" tools/binder-mk.c
        "$CC" -O2 -Wall -Wextra -o "$APPDIR/tools/binder-ver" tools/binder-ver.c
        chmod 0755 "$APPDIR/tools/binder-mk" "$APPDIR/tools/binder-ver"
    else
        echo "warning: no C compiler, shipping without helpers" >&2
    fi
    if command -v pkg-config >/dev/null && pkg-config --exists sdl2 2>/dev/null; then
        echo "-- building the display client (SDL2)"
        # shellcheck disable=SC2046
        "$CC" -O2 -Wall -o "$APPDIR/frontend/hwc_client" frontend/hwc_client.c \
              -Ihwc/include $(pkg-config --cflags --libs sdl2)
        chmod 0755 "$APPDIR/frontend/hwc_client"
        HAS_SDL=1
    else
        echo "note: SDL2 headers absent, hwc_client not prebuilt (CLI builds it on use)"
    fi
fi

# ---- documentation -------------------------------------------------------
install -m0644 README.md "$DOCDIR/README.md"
mkdir -p "$DOCDIR/docs"
for f in docs/*.md; do install -m0644 "$f" "$DOCDIR/docs/"; done
install -m0644 LICENSE "$DOCDIR/copyright"
cat > "$DOCDIR/changelog" <<EOF
$PKG ($VERSION) unstable; urgency=medium

  * arm64 Android userspace on x86_64: qemu-user translation plus the host
    kernel's binderfs/memfd, with an unmodified LineageOS 20 arm64 image.
  * tools: pure-python super.img unpacker and APK/AXML inspector.
  * hwc: software HWC2 composer module, host client over SDL2 + uinput.
  * packaging: this Debian package.

 -- $MAINTAINER  $(date -R)
EOF
gzip -9n "$DOCDIR/changelog"
install -m0644 packaging/qandroid.1 "$MANDIR/qandroid.1"
gzip -9n "$MANDIR/qandroid.1"
chmod 0755 "$DOCDIR"

# ---- control -------------------------------------------------------------
DEPS="python3 (>= 3.8), qemu-user-static, e2fsprogs"
if [ "$WITH_BINARIES" = 1 ] && [ -e "$APPDIR/tools/binder-mk" ]; then
    DEPS="$DEPS, libc6"
fi
if [ "$HAS_SDL" = 1 ]; then
    DEPS="$DEPS, libsdl2-2.0-0"
fi

INSTALLED_SIZE=$(python3 -c "
import os,sys
t=0
for b,_d,f in os.walk('$STAGE'):
    if '/DEBIAN' in b: continue
    for n in f:
        try: t+=os.path.getsize(os.path.join(b,n))
        except OSError: pass
print((t+1023)//1024)")
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: ${INSTALLED_SIZE:-0}
Section: misc
Priority: optional
Homepage: https://github.com/R0GUEEE/qandroid
Depends: $DEPS
Recommends: libsdl2-2.0-0, curl, android-sdk-libsparse-utils, binfmt-support
Suggests: gcc | cc, make
Description: run arm64 Android APKs on x86_64 Linux
 qandroid runs arm64 Android applications on an x86_64 Linux host by combining
 three pieces: qemu-aarch64-user for instruction translation, the host kernel
 for the architecture-independent half of the Android ABI (binderfs, memfd,
 futex, namespaces), and an unmodified arm64 Android userspace on top.
 .
 Because the guest is a real Android userspace, bionic, ART and SurfaceFlinger
 are the platform's own code - nothing about Android is reimplemented, only the
 instruction set is translated.
 .
 Ships the qandroid CLI, a software HWC2 composer module for the guest, an SDL2
 display client with a uinput touchscreen, a pure-python super.img unpacker, an
 APK/AXML inspector, and the qemu patches the layer needs.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
case "$1" in
  configure)
    # Deliberately non-intrusive: qandroid never edits the system's binfmt
    # entries on its own, it only reports what is missing.
    echo "qandroid: 'qandroid doctor' checks prerequisites, 'qandroid setup'"
    echo "          fetches an arm64 image and builds a rootfs (needs ~2 GiB)."
    ;;
esac
exit 0
EOF
chmod 0755 "$STAGE/DEBIAN/postinst"

ln -sf "../lib/$PKG/qandroid" "$STAGE/usr/bin/qandroid"

# md5sums (python: busybox find has no -printf)
python3 - "$STAGE" <<'PY'
import hashlib, os, sys
stage = sys.argv[1]
lines = []
for base, _dirs, files in os.walk(stage):
    if base.startswith(os.path.join(stage, "DEBIAN")):
        continue
    for f in sorted(files):
        full = os.path.join(base, f)
        if os.path.islink(full):
            continue          # md5sums lists file contents; symlinks have none
        rel = os.path.relpath(full, stage)
        h = hashlib.md5()
        with open(full, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        lines.append("%s  %s\n" % (h.hexdigest(), rel))
with open(os.path.join(stage, "DEBIAN", "md5sums"), "w") as fh:
    fh.writelines(sorted(lines))
PY
chmod 0644 "$STAGE/DEBIAN/md5sums"

# ---- build ---------------------------------------------------------------
mkdir -p "$OUT"
DEB=$OUT/${PKG}_${VERSION}_${ARCH}.deb
echo "== dpkg-deb =="
dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
echo
dpkg-deb --info "$DEB"
echo
echo "contents (first 20 entries):"
dpkg-deb --contents "$DEB" > "$OUT/.contents.txt"
sed -n '1,20p' "$OUT/.contents.txt"
echo "..."
rm -f "$OUT/.contents.txt"
(cd "$OUT" && sha256sum "$(basename "$DEB")")
echo
echo "install with: sudo apt install ./$(basename "$DEB")"
