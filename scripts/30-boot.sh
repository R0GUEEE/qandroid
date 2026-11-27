#!/bin/sh
# 30-boot.sh - start the arm64 Android userspace under QEMU on an x86_64 host.
#
#   ./scripts/30-boot.sh [ROOTFS] [init|manual]
#
# What the host has to provide (this is the "compatibility" half):
#   * proc, sys, a private /dev with binderfs nodes   -> kernel, arch independent
#   * a PID namespace so the guest's init can be PID 1 -> kernel namespaces
#   * QEMU aarch64 TCG for instruction translation     -> qemu-user-static
# Everything above that (bionic, ART, SurfaceFlinger, binder IPC semantics) is
# the untouched arm64 Android userspace inside ROOTFS.
set -eu

ROOTFS="${1:-${QANDROID_ROOTFS:-$HOME/.local/share/qandroid/rootfs}}"
MODE="${2:-init}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOPDIR=$(dirname "$HERE")

[ -d "$ROOTFS/system" ] || { echo "no rootfs at $ROOTFS (run: ./qandroid rootfs build)"; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "boot needs root (namespaces, mounts)"; exit 1; }
command -v qemu-aarch64-static >/dev/null || { echo "missing qemu-aarch64-static"; exit 1; }

mkdir -p "$ROOTFS"/{proc,sys,dev,data,tmp}
mountpoint -q "$ROOTFS/proc" || mount -t proc proc "$ROOTFS/proc"
mountpoint -q "$ROOTFS/sys"  || { mount --rbind /sys "$ROOTFS/sys" && mount --make-rslave "$ROOTFS/sys"; }
mountpoint -q "$ROOTFS/dev"  || { mount --rbind /dev "$ROOTFS/dev" && mount --make-rslave "$ROOTFS/dev"; }

# binderfs mountpoint must be created *after* the /dev rbind: that mount hides
# whatever was in the image's /dev directory, including the directory itself.
mkdir -p "$ROOTFS/dev/binderfs" "$ROOTFS/dev/socket" "$ROOTFS/dev/__properties__"
if ! mountpoint -q "$ROOTFS/dev/binderfs"; then
    if ! mount -t binder binder "$ROOTFS/dev/binderfs" 2>/dev/null; then
        echo "warning: binderfs unavailable (modprobe binder_linux?); the guest will" >&2
        echo "         start but every binder service will fail to open /dev/binder" >&2
    fi
fi
if [ ! -x "$TOPDIR/tools/binder-mk" ]; then
    echo "error: tools/binder-mk is missing - run 'make tools' (it is gitignored," >&2
    echo "       so a fresh clone does not have it). Without it there is no" >&2
    echo "       /dev/binder and every Android service aborts at startup." >&2
    exit 1
fi
for n in binder hwbinder vndbinder; do
    "$TOPDIR/tools/binder-mk" "$ROOTFS/dev/binderfs" "$n" || echo "  binder-mk $n failed" >&2
    if [ -e "$ROOTFS/dev/binderfs/$n" ] && [ ! -e "$ROOTFS/dev/$n" ]; then
        : > "$ROOTFS/dev/$n"                  # create the mountpoint
        mount --bind "$ROOTFS/dev/binderfs/$n" "$ROOTFS/dev/$n"
    fi
done

# APEX: these images ship *flattened* apexes under /system/apex/<name>/ while
# the runtime expects them at /apex/<name> (that is where /system/bin/linker64
# points). Mounting each one is the job init does on a device; here the host
# does it, which is fine because it is a kernel-level operation, not a guest one.
if [ -d "$ROOTFS/system/apex" ]; then
    made=0
    for d in "$ROOTFS"/system/apex/*/; do
        [ -d "$d" ] || continue
        name=$(basename "$d")
        target="$ROOTFS/apex/$name"
        mkdir -p "$target"
        if mountpoint -q "$target" 2>/dev/null; then
            continue
        fi
        if mount --bind "$d" "$target" 2>/dev/null; then
            made=$((made + 1))
        fi
    done
    echo "apex: bound $made flattened apex(es) into /apex"
    if [ -x "$ROOTFS/system/bin/linker64" ] || [ -e "$ROOTFS/system/bin/linker64" ]; then
        :
    fi
fi

# ld.config.txt: on a device, init runs /system/bin/linkerconfig which builds it
# from vndk metadata in /system/etc. Container images ship neither the metadata
# nor a pre-generated file, and without it bionic resolves every dependency
# against /system/lib64 only - libandroidicu (i18n APEX), libnetd_updatable
# (tethering APEX) and the system_ext HALs all go missing.
#
# Two details that matter:
#   * bionic picks the config *section* from the executable's directory via the
#     dir.* prefixes, so those lines are required, not decoration;
#   * the format has no comment syntax - a leading '#' line makes the whole file
#     unparseable and bionic silently falls back to the built-in default.
if [ ! -s "$ROOTFS/linkerconfig/ld.config.txt" ]; then
    mkdir -p "$ROOTFS/linkerconfig"
    base="/system/lib64:/system/system_ext/lib64:/product/lib64:/vendor/lib64:/odm/lib64"
    apex=""
    for d in "$ROOTFS"/system/apex/*/lib64; do
        [ -d "$d" ] || continue
        n=$(basename "$(dirname "$d")")
        apex="$apex:/apex/$n/lib64:/system/apex/$n/lib64"
    done
    paths="$base$apex"
    cat > "$ROOTFS/linkerconfig/ld.config.txt" <<EOF
dir.system = /system/bin
dir.system.lib = /system/lib
dir.system.lib64 = /system/lib64
dir.system_ext = /system/system_ext/bin
dir.system_ext.lib = /system/system_ext/lib
dir.system_ext.lib64 = /system/system_ext/lib64
dir.vendor = /vendor/bin
dir.vendor.lib = /vendor/lib
dir.vendor.lib64 = /vendor/lib64
dir.product.lib64 = /product/lib64
dir.odm.lib64 = /odm/lib64

[system]
namespace.default.isolated = false
namespace.default.search.paths = $paths
namespace.default.permitted.paths = $paths:/apex:/system/apex:/data

[vendor]
namespace.default.isolated = false
namespace.default.search.paths = $paths
namespace.default.permitted.paths = $paths:/apex:/system/apex:/data

[system_ext]
namespace.default.isolated = false
namespace.default.search.paths = $paths
namespace.default.permitted.paths = $paths:/apex:/system/apex:/data
EOF
    echo "linkerconfig: wrote /linkerconfig/ld.config.txt ($(echo "$paths" | tr ':' '\n' | wc -l) search paths)"
fi

# the dynamic linker is what every other binary needs; check it before handing
# over to the guest, because the failure mode otherwise is a wall of ENOENT
if [ -e "$ROOTFS/system/bin/linker64" ] && [ ! -e "$ROOTFS/apex/com.android.runtime/bin/linker64" ]; then
    echo "warning: /system/bin/linker64 dangles - /apex/com.android.runtime is not mounted." >&2
    echo "         flatten-mount the apexes above, or run with real init." >&2
fi

echo "binder mounts: $(grep -c binder /proc/mounts 2>/dev/null || echo 0)"
grep binder /proc/mounts 2>/dev/null | sed 's/^/  /' || true
ls -l "$ROOTFS/dev/binder" "$ROOTFS/dev/hwbinder" "$ROOTFS/dev/vndbinder" 2>/dev/null | sed 's/^/  /' || true

install -m0755 "$HERE/guest-boot.sh" "$ROOTFS/system/bin/qandroid-boot"

ENVV="ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_STORAGE=/storage \
ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n \
ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
PATH=/system/bin:/system/xbin:/vendor/bin:/sbin:/usr/bin:/bin \
HOME=/data LOGNAME=root USER=root TERM=${TERM:-xterm}"

# QANDROID_STRACE=1 -> every guest syscall qemu cannot or should not pass through
# verbatim lands in the log; essential when a guest ioctl fails with ENOTTY.
if [ -n "${QANDROID_STRACE:-}" ]; then
    ENVV="$ENVV QEMU_STRACE=$QANDROID_STRACE"
fi

# Direct mode: run a single guest process under an explicitly chosen qemu.
# binfmt is left alone - this is how the patched qemu gets exercised without
# touching the host's binary formats. Guest paths stay correct because we are
# inside the chroot; qemu is copied in so it is reachable there.
if [ -n "${QANDROID_QEMU_BIN:-}" ]; then
    cp -f "$QANDROID_QEMU_BIN" "$ROOTFS/qemu-aarch64-static"
    chmod 0755 "$ROOTFS/qemu-aarch64-static"
    DIRECT_CMD="${QANDROID_DIRECT_CMD:-/system/bin/servicemanager}"
    echo "direct mode: $DIRECT_CMD under $QANDROID_QEMU_BIN"
    exec unshare --pid --mount --fork --mount-proc \
        env -i $ENVV \
        chroot "$ROOTFS" /qemu-aarch64-static "$DIRECT_CMD"
fi

echo "booting $ROOTFS (mode=$MODE)"
# --mount keeps init's mount acrobatics inside our namespace; --mount-proc gives
# the guest an arch-correct /proc even though QEMU emulates the syscalls.
exec unshare --pid --mount --fork --mount-proc \
    env -i $ENVV \
    chroot "$ROOTFS" /system/bin/sh /system/bin/qandroid-boot "$MODE"
