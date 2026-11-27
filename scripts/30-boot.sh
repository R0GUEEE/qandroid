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

mkdir -p "$ROOTFS"/{proc,sys,dev,dev/binderfs,dev/socket,dev/__properties__,data,tmp}
mountpoint -q "$ROOTFS/proc" || mount -t proc proc "$ROOTFS/proc"
mountpoint -q "$ROOTFS/sys"  || mount --rbind /sys "$ROOTFS/sys"
mount --make-rslave "$ROOTFS/sys"
mountpoint -q "$ROOTFS/dev"  || mount --rbind /dev "$ROOTFS/dev"

# binderfs: three devices, one per byte-stream domain
if ! mountpoint -q "$ROOTFS/dev/binderfs"; then
    mount -t binder binder "$ROOTFS/dev/binderfs"
fi
for n in binder hwbinder vndbinder; do
    "$TOPDIR/tools/binder-mk" "$ROOTFS/dev/binderfs" "$n" >/dev/null 2>&1 || true
    if [ -e "$ROOTFS/dev/binderfs/$n" ] && [ ! -e "$ROOTFS/dev/$n" ]; then
        : > "$ROOTFS/dev/$n"                  # create the mountpoint
        mount --bind "$ROOTFS/dev/binderfs/$n" "$ROOTFS/dev/$n"
    fi
done

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
