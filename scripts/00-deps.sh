#!/bin/sh
# 00-deps.sh - host prerequisites for qandroid on Ubuntu 22.04/24.04 x86_64.
#
# Installs: qemu-user-static (aarch64 TCG), binfmt registration, e2fsprogs
# (debugfs for unprivileged ext4 extraction), and the misc tools the CLI shells out to.
set -eu

say() { printf '\033[1m== %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO="sudo"

say "host check"
[ "$(uname -m)" = "x86_64" ] || echo "warning: host is $(uname -m), qandroid is tuned for x86_64"
echo "kernel: $(uname -r)"

say "packages"
$SUDO apt-get update -qq
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    qemu-user-static binfmt-support e2fsprogs android-sdk-libsparse-utils \
    python3 curl unzip zip file procps psmisc lsof \
    build-essential libc6-dev

say "binfmt_misc"
if [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    echo "qemu-aarch64 binfmt already registered"
else
    # binfmt-support ships update-binfmts; qemu-user-static ships the registrations
    $SUDO update-binfmts --enable qemu-aarch64 2>/dev/null || true
    if [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
        $SUDO sh -c 'echo ":qemu-aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:PF" > /proc/sys/fs/binfmt_misc/register'
    fi
fi
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] && echo "binfmt aarch64: ok" || echo "binfmt aarch64: FAILED"

say "kernel capabilities this layer depends on"
grep -qw binderfs /proc/filesystems 2>/dev/null \
    && echo "binderfs: ok" \
    || echo "binderfs: MISSING - need CONFIG_ANDROID_BINDERFS (Ubuntu's generic kernel has it; WSL does not)"
echo "memfd_create: $(grep -qw memfd_create /proc/kallsyms && echo ok || echo 'unknown - need kernel >= 3.17')"
[ -e /dev/ashmem ] && echo "ashmem: present (legacy path, only needed for Android <= 10 images)" \
                   || echo "ashmem: absent (fine for Android 12+ images, which use memfd)"

say "build the binder node helper"
make -C "$(dirname "$0")/.." tools/binder-mk

say "done - next: ./qandroid image list , then ./qandroid image fetch"
