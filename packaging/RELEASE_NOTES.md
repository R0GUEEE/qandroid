# qandroid @TAG@

Run arm64 Android APKs on x86_64 Linux: `qemu-aarch64-user` translates the
instruction set, the host kernel supplies the architecture-independent half of
the Android ABI (binderfs, memfd, futex, namespaces), and an unmodified arm64
LineageOS 20 userspace sits on top. bionic, ART and SurfaceFlinger are the
platform's own code — nothing about Android is reimplemented.

## Assets

| file | what it is |
|---|---|
| `install.sh` | one-shot installer (`curl … \| sudo sh`, or pin this exact file) |
| `qandroid_@VERSION@_amd64.deb` | Debian/Ubuntu package: CLI, tools, scripts, qemu patches, man page, host helpers |
| `hwcomposer.qandroid.so` | software HWC2 composer module for the **guest** (arm64, build inside AOSP, see `docs/HWC.md`) |
| `libabshim.so` | bionic symbol shim for standalone arm64 ELFs (arm64) |
| `binder-mk`, `binder-ver` | host helpers: create binderfs nodes / prove a binder node works |
| `hwc_client` | host display client (x86-64, SDL2 + uinput touchscreen) |
| `qandroid-@VERSION@.tar.gz` | full source |
| `SHA256SUMS.txt` | checksums for everything above |

## Quick start

```sh
sudo apt install ./qandroid_@VERSION@_amd64.deb
qandroid doctor      # host prerequisites
qandroid setup       # fetch an arm64 image (~1 GiB) and build a rootfs
sudo qandroid boot
qandroid display
```

## What is verified, and what is not

Verified on CI against the real 870 MiB arm64 image:

* arm64 Android binaries execute under `qemu-aarch64-static` — the bionic
  linker, `sh`, `netd` and `audioserver` reach their own code
* `binderfs` mounts, nodes are allocated and the guest opens `/dev/binder`
* flattened APEXes bind into `/apex`; APEX and `system_ext` libraries resolve
  once `/linkerconfig/ld.config.txt` exists
* binder **transactions** work under a qemu built with `patches/apply.sh`:
  `servicemanager` runs for the full observation window instead of aborting
* the .deb installs, runs, and removes cleanly; lintian reports nothing

Not verified, and not claimed:

* a full boot to a working zygote (needs the patched qemu wired into
  `binfmt_misc`; see `docs/BRINGUP.md` for why that is not done for you)
* `pm install` of a real APK, and the HWC client end to end
* performance numbers

32-bit (`armeabi-v7a`) APKs are out of scope, and the guest has no GPU: the
composer composites on the CPU.

## New in 0.3.1

* `install.sh` — installs the CLI and its dependencies on Debian/Ubuntu (release
  `.deb`, checksum verified) and on any other distribution (source install with
  that distro's packages). `--uninstall`, `--dry-run`, `--version`, `--file`,
  `--source`, `--with-image`.
* Alpine: `debugfs` comes from `e2fsprogs-extra`, and `qemu-aarch64` (not
  `-static`) is what the repositories ship — the CLI accepts either.
* Uninstall now removes everything both install paths can leave behind and
  fails if `qandroid` still resolves.
* `curl | sh` is detected: the script will not try to re-exec itself under sudo
  when it has no file to re-exec.

## Notable fixes in 0.3.0

* rootfs extraction handles system-as-root images and flattened APEXes
* `ld.config.txt` is generated at boot — without it every APEX library is invisible
* `BINDER_CTL_ADD` uses `_IOWR`, and qemu needs a `'b'`-ioctl passthrough
* packaging: xz compression, stripped binaries, correct `libc6`/SDL2 dependencies

Full evidence trail: `docs/BRINGUP.md`.
