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
for n in binder hwbinder vndbinder; do
    "$TOPDIR/tools/binder-mk" "$ROOTFS/dev/binderfs" "$n" >/dev/null 2>&1 || true
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

# the dynamic linker is what every other binary needs; check it before handing
# over to the guest, because the failure mode otherwise is a wall of ENOENT
if [ -e "$ROOTFS/system/bin/linker64" ] && [ ! -e "$ROOTFS/apex/com.android.runtime/bin/linker64" ]; then
    echo "warning: /system/bin/linker64 dangles - /apex/com.android.runtime is not mounted." >&2
    echo "         flatten-mount the apexes above, or run with real init." >&2
fi

install -m0755 "$HERE/guest-boot.sh" "$ROOTFS/system/bin/qandroid-boot"

ENVV="ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_STORAGE=/storage \
ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n \
ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
PATH=/system/bin:/system/xbin:/vendor/bin:/sbin:/usr/bin:/bin \
HOME=/data LOGNAME=root USER=root TERM=${TERM:-xterm}"

echo "booting $ROOTFS (mode=$MODE)"
# --mount keeps init's mount acrobatics inside our namespace; --mount-proc gives
# the guest an arch-correct /proc even though QEMU emulates the syscalls.
exec unshare --pid --mount --fork --mount-proc \
    env -i $ENVV \
    chroot "$ROOTFS" /system/bin/sh /system/bin/qandroid-boot "$MODE"
