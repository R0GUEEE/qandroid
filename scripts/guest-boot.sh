#!/bin/sh
# guest-boot.sh - runs INSIDE the arm64 guest (chroot), as root.
#
# Two modes:
#   init    (default) hand off to the real /system/bin/init, which sets up the
#           property area, /dev, binder devices and every service from *.rc.
#           This is the faithful path: properties exist, zygote starts normally.
#   manual  start the minimum service set by hand.  Faster to debug, but there is
#           no property service, so ART falls back to build.prop defaults and
#           many framework checks degrade.  Training wheels only.
set -u

MODE="${1:-init}"
LOG=/data/local/tmp/qandroid-boot.log
mkdir -p /data/local/tmp 2>/dev/null || true
exec >>"$LOG" 2>&1
echo "=== qandroid guest boot $(date -u +%FT%TZ) mode=$MODE ==="

export ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_STORAGE=/storage
export ANDROID_ART_ROOT=/apex/com.android.art ANDROID_I18N_ROOT=/apex/com.android.i18n
export ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
export PATH=/system/bin:/system/xbin:/vendor/bin:/sbin:/usr/bin:/bin

# /dev is prepared by the host (bind mounts + binderfs); only fill gaps with
# real character nodes - never with regular files, which would shadow them.
mknode() {
    [ -e "/dev/$1" ] || mknod "/dev/$1" "$2" "$3" "$4" 2>/dev/null || true
}
mknode null    c 1 3
mknode zero    c 1 5
mknode full    c 1 7
mknode random  c 1 8
mknode urandom c 1 9
mknode tty     c 5 0
mkdir -p /dev/socket /dev/__properties__ /data/property

if [ "$MODE" = "init" ]; then
    echo "exec /system/bin/init"
    exec /system/bin/init
fi

spawn() {
    name=$1
    shift
    echo "start $name: $*"
    "$@" &
    echo $! >"/data/local/tmp/$name.pid"
    sleep 1
}

# Order is AIDL servicemanager -> HIDL hwservicemanager -> everything else.
[ -x /system/bin/servicemanager ]     && spawn servicemanager /system/bin/servicemanager
[ -x /vendor/bin/hwservicemanager ]   && spawn hwservicemanager /vendor/bin/hwservicemanager
[ -x /vendor/bin/vndservicemanager ]  && spawn vndservicemanager /vendor/bin/vndservicemanager
[ -x /system/bin/surfaceflinger ]     && spawn surfaceflinger /system/bin/surfaceflinger
[ -x /system/bin/audioserver ]        && spawn audioserver /system/bin/audioserver
[ -x /system/bin/storaged ]           && spawn storaged /system/bin/storaged
[ -x /system/bin/netd ]               && spawn netd /system/bin/netd
[ -x /system/bin/zygote64 ]           && spawn zygote64 /system/bin/zygote64

echo "=== service set started; tail -f $LOG inside the guest ==="
wait
