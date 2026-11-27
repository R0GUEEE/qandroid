[![ci](https://github.com/R0GUEEE/qandroid/actions/workflows/ci.yml/badge.svg)](https://github.com/R0GUEEE/qandroid/actions/workflows/ci.yml)
[![guest-smoke](https://github.com/R0GUEEE/qandroid/actions/workflows/guest-smoke.yml/badge.svg)](https://github.com/R0GUEEE/qandroid/actions/workflows/guest-smoke.yml)

# qandroid — arm64 Android on x86_64 Linux

A compatibility layer that runs **arm64 Android APKs on an x86_64 Ubuntu host**
by combining three things that already exist and are well understood, and
adding only the glue that is actually missing:

```
      ┌──────────────────────────────────────────────────────────┐
      │  arm64 Android userspace  (LineageOS 20 / Waydroid image)│  ← unmodified
      │  bionic · libhwbinder · ART · SurfaceFlinger · zygote    │
      └──────────────────────────────────────────────────────────┘
              │ HWC2                          ▲ evdev (uinput)
      ┌───────▼───────────────────────────────┴──────────────────┐
      │ hwcomposer.qandroid.so → socket+fd → hwc_client (SDL)    │  ← display
      └──────────────────────────────────────────────────────────┘
      ┌──────────────────────────────────────────────────────────┐
      │  qemu-aarch64-user (TCG)   arm64 instructions → x86_64   │  ← distro package
      │  + one prctl patch (VM naming)                           │
      └──────────────────────────────────────────────────────────┘
      ┌──────────────────────────────────────────────────────────┐
      │  host kernel: binderfs · memfd · futex · namespaces      │  ← arch independent
      └──────────────────────────────────────────────────────────┘
```

The point: **`binderfs`, `memfd`, `futex` and namespaces are not
architecture-specific.** The half of Android that lives in the kernel already
works on x86_64 — what does not work is arm64 *machine code*. So instead of
reimplementing the Android runtime (what libhoudini/libndk-translation do,
proprietarily), you translate the instruction set and keep the runtime.

That means the Android side is the *real* Android: real bionic, real ART, real
binder semantics, real `pm`/`am`. Nothing about Android is being emulated or
approximated. Only `mov`, `add` and friends are.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/R0GUEEE/qandroid/main/install.sh | sudo sh
```

The script picks the right path for the host: Debian/Ubuntu get the release
`.deb` (checksum verified, dependencies resolved by apt), everything else gets a
source install into `/opt/qandroid` with that distro's packages. It never edits
`binfmt_misc`, never downloads the image, and never touches your home directory.

```sh
sh install.sh --version 0.3.0        # pin a release (default: latest)
sh install.sh --file ./pkg.deb       # install a local .deb
sh install.sh --source               # force the source install (any distro)
sh install.sh --with-image           # also run 'qandroid setup' (~1 GiB)
sh install.sh --uninstall [--purge-data]
```

Then, either way:

```sh
qandroid doctor     # confirm the host is ready (binderfs, qemu, binfmt)
qandroid setup      # fetch an arm64 image and build a rootfs
sudo qandroid boot
qandroid display    # HWC2 window (SDL2)
```

### Or install the package by hand

```sh
sudo apt install ./qandroid_0.3.0_amd64.deb     # from a release or the CI artifact
```

The package installs `/usr/lib/qandroid/` (CLI, tools, scripts, qemu patches,
HWC sources, man page), symlinks `/usr/bin/qandroid`, and pulls
`python3`, `qemu-user-static` and `e2fsprogs` as dependencies. It never touches
the system's binfmt entries: `qandroid doctor` reports what is missing and
`qandroid setup` does the rest. Build your own with `make deb`
(`packaging/build-deb.sh --arch all --no-binaries` for a source-only package).

## Quickstart (Ubuntu 22.04 / 24.04, x86_64)

```sh
./scripts/00-deps.sh                # qemu-user-static, binfmt, e2fsprogs, tools
./qandroid doctor                   # verify kernel + binfmt + qemu

./qandroid image fetch --abi arm64  # ~870 MiB, newest LineageOS 20 arm64 image
./qandroid rootfs build             # system.img/vendor.img → ~/.local/share/qandroid/rootfs
sudo ./qandroid boot                # PID namespace + binderfs + guest init
sudo ./qandroid apk install app.apk
sudo ./qandroid apk start com.example.app
./qandroid logs -f
```

Display — the HWC2 backend (see `docs/HWC.md`):

```sh
make client                       # build the host window (needs libsdl2-dev)
./frontend/hwc_client --socket ~/.local/share/qandroid/hwc.sock
```

Fallback if the composer module is not installed yet:

```sh
sudo ./qandroid viewer            # polls screencap, click = tap, drag = swipe
```

Inspect an APK without any guest at all:

```sh
./qandroid apk info app.apk       # package, sdk levels, ABIs, native libs
./tools/apk.py libs app.apk --abi arm64-v8a -o out/
```

Build the two display halves:

```sh
make hwc-check          # composer module: syntax-clean standalone
make -C hwc             # composer module for arm64 (cross compiler)
make client             # host window (SDL2)
```

## Why this shape rather than the obvious alternatives

| Approach | Runs arm64 APKs on x64? | Why / why not |
|---|---|---|
| Waydroid / Anbox on x86_64 | **no** | the Android userspace is the host's architecture; arm64 libs need a per-arch translator they do not ship |
| Android Studio emulator (arm64 image) | yes | full system emulation: whole kernel + devices. Boots in minutes, ~10× slower than user-mode TCG, and it is a VM, not a Linux process tree |
| libhoudini / libndk-translation | yes | proprietary, bundle of undocumented per-arch libc shims, redistribution is not available |
| Box64/FEX + Android libs | partly | those translate x86 binaries *to* arm64 (or emulators), not Android ABIs; no binder, no bionic |
| **this** | **yes** | qemu-user TCG for instructions + host kernel for the arch-independent ABI + untouched arm64 Android userspace |

`qemu-system-aarch64` also works and is a one-liner, but it virtualises a whole
machine; here the guest shares the host kernel, so file I/O, sockets and binder
are native speed and the layer is ~200 lines of glue instead of a device model.

## Status — verified vs. not

Verified in this repo (unit tests, runnable now):

* `tools/lpunpack.py` — parses real `LP_METADATA_*` dynamic partition tables
  (geometry at 4096, slot 0 at 8192, CRC32 on header and tables) and extracts
  every LINEAR extent. Tested against synthetic `super.img` fixtures incl. a
  truncated-extent failure case.
* `tools/apk.py` — zip + binary `AndroidManifest.xml` (AXML string pool, UTF-8
  and UTF-16, resource-mapped attributes); reports package, versionCode, SDK
  levels, permissions, ABIs and extracts native libs.
* `qandroid image list/fetch` — live against the Waydroid SourceForge mirror;
  reads real filenames, sizes and md5 from the RSS feed.
* `patches/apply.sh` — idempotent qemu patcher, tested against a fake tree
  (applies once, refuses to apply twice, fails loudly on a moved anchor).
* `tools/binder-mk`, `shim/abshim.c` — compile clean with `-Wall -Wextra`.
* `hwc/src/hwcomposer_qandroid.c` — full `hwc2_device_t` implementation,
  syntax-clean against the vendored shim headers; the guest/host protocol
  (168-byte frame header, damage rects, ack backpressure) is covered by tests
  including a live `SCM_RIGHTS` round trip where the kernel supports it.
* `frontend/hwc_client.c` — SDL2 window + uinput touchscreen, builds clean.

Brought up on GitHub Actions (see `docs/BRINGUP.md` for the run-by-run
evidence):

* arm64 Android binaries execute under `qemu-aarch64-static` — the bionic
  linker, `sh`, `netd`, `audioserver` all reach their own code
* `binderfs` mounts, nodes are created and the guest opens `/dev/binder`
* flattened APEX binding works; APEX and `system_ext` libraries resolve once
  `/linkerconfig/ld.config.txt` exists

* binder **IPC works under a patched qemu**: `servicemanager` runs until the
  time-box kills it (exit 124) instead of aborting on `BINDER_VERSION`. The
  patch is in `patches/apply.sh`; wiring it into `binfmt_misc` for a full boot is
  what is left (see `docs/BRINGUP.md`, it cannot be done safely on a hosted
  CI runner)

Still not verified:

* booting to a working zygote, `pm install` of a real APK
* SurfaceFlinger + the HWC client end to end
* performance numbers

The project is structured so those are *runs*, not rewrites: `qandroid doctor`
tells you exactly which prerequisite is missing before you hit it.

## CI

| workflow | trigger | what it does |
|---|---|---|
| `ci.yml` | push, PR, tag | unit tests on Python 3.10 + 3.12, static lint, cross-build of the arm64 composer module and bionic shim, host client against SDL2, source tarball + SHA256 |
| `guest-smoke.yml` | manual | downloads the real 870 MiB arm64 image, builds a rootfs, boots the guest time-boxed under QEMU and uploads every log. A diagnostic harness, not a gate |

The build job is the useful one to watch: it proves the composer module is
really `ARM aarch64`, that the shim compiles for the guest, and that the host
client links against SDL2 on a clean x86_64 image — the three things that were
unverifiable from the machine this was written on.

## Repo layout

```
qandroid                 CLI: doctor, image, rootfs, apk, boot, logs, viewer
scripts/00-deps.sh       host packages + binfmt registration
scripts/30-boot.sh       namespaces, binderfs, mounts, guest init
scripts/guest-boot.sh    in-guest bootstrap (init mode / manual mode)
tools/lpunpack.py        pure-python super.img / logical partition unpacker
tools/apk.py             APK + AXML inspector, ABI lib extractor
tools/binder-mk.c        BINDERFS_CTL_ADD node creator
patches/apply.sh         qemu-user patch (prctl PR_SET_VMA) + ABI audit
shim/abshim.c            bionic symbol shim for standalone arm64 ELFs
frontend/viewer.py       screencap-based display/input frontend
hwc/                     software HWC2 composer module (guest, arm64)
hwc/include/             wire protocol + shim headers for standalone builds
frontend/hwc_client.c    host window: SDL2 + uinput touchscreen
tests/                   20 unit tests, no guest required
docs/ARCHITECTURE.md     where each ABI boundary is crossed
docs/LIMITS.md           what still breaks and why
```

## Roadmap

1. ~~**HWC backend**~~ — done in v0.2: `hwc/` + `frontend/hwc_client.c`, see
   `docs/HWC.md`. Next: YUV/video layer support and rotated-layer offload.
2. **Property service before init** — let `manual` mode seed `/dev/__properties__`
   so debugging does not require a full init.
3. **arm32 guest** — a second `qemu-arm` guest for `armeabi-v7a`-only APKs.
4. **`/dev/ashmem` over FUSE** — Android ≤ 11 images on kernels ≥ 5.18.
5. **Snapshot/restore of the TCG block cache** — cold start is dominated by
   translation, not by Android.
