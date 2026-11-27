# Bring-up log: what the CI runs actually proved

Each row is a real `guest-smoke` run against the 870 MiB arm64 LineageOS 20
image (system + vendor partitions). This is the evidence trail, including the
things that were assumptions before it existed.

| # | Change under test | Result |
|---|---|---|
| 1 | first dispatch | died downloading (single urllib stream from the mirror stalled 25 min) → fetch rewritten on `curl --continue-at` |
| 2 | curl fetch | rootfs build failed: `FileNotFoundError: rootfs/etc/qandroid-rootfs` |
| 3 | system-as-root extraction | rootfs builds, 40 top-level entries, `init`/`sh`/`app_process64` resolve |
| 4 | mount ordering fix | guest runs: bionic linker prints its own warnings, services attempt to start |
| 5 | vendor image + ld.config | `netd` and `audioserver` link and execute; `libnetd_updatable`, `libandroidicu` resolve from APEX |
| 6 | `binder-mk` built in CI, `_IOWR` fix | binderfs mounts, nodes `238:1-3` created and bind-mounted into `/dev` |
| 7 | `binder-ver` host probe | host: `binder protocol 8` — guest: `BINDER_VERSION → ENOTTY` |

## What that establishes

**Working, by observation (not by argument):**

* arm64 Android ELF binaries execute on x86_64 Linux through `qemu-aarch64-static`
  and `binfmt_misc` — the linker, `sh`, `netd`, `audioserver`, `surfaceflinger`
  all reach their own `main()` or die inside Android code, not inside the loader.
* the host kernel supplies the arch-independent half: `binderfs` mounts and
  `BINDERFS_CTL_ADD` allocates devices; the guest opens `/dev/binder` as a
  character device.
* flattened APEX binding works: 25 apexes bound into `/apex`, so
  `/system/bin/linker64` resolves.
* dependency resolution through APEX and `system_ext` works once
  `/linkerconfig/ld.config.txt` exists — `netd` gets past `libnetd_updatable`,
  `audioserver` past `libandroidicu`.

**Blocking, with the cause isolated:**

`BINDER_VERSION` (`0xc0046209`) succeeds from the host and fails with `ENOTTY`
from inside the guest. Same node, same driver, same run. So the request is being
rejected *before* it reaches the kernel — `linux-user` answers `ENOTTY` for
ioctls it has no translation entry for, and it has none for `BINDER_*`.

`patches/apply.sh` now adds a two-line guard at the top of `do_ioctl` that hands
every `'b'`-type ioctl to the host unchanged. Applying it requires a qemu built
from source (see below); the distro `qemu-user-static` cannot be fixed in place.

## Bugs found and fixed along the way

| Bug | Why it was invisible |
|---|---|
| `rootfs build` nested a system-as-root image under `rootfs/system` | the extraction "succeeded"; only the marker write failed |
| `mount --rbind /dev` hid the `dev/binderfs` mountpoint | the error was `mount point does not exist`, two lines after a successful mkdir |
| `BINDER_CTL_ADD` used `_IOW` instead of `_IOWR` | ioctl direction bits are part of the command number; the kernel answers `EINVAL`, which reads like a bad struct |
| `tools/binder-mk` is gitignored and CI never built it | the boot script called a missing binary with `|| true` |
| `ld.config.txt` started with a `#` comment | bionic's config format has no comment syntax; it silently falls back to `/system/lib64` only |
| system image fetched without the vendor partition | SurfaceFlinger failed on `vendor.waydroid.display@1.0.so` — which ships in `system_ext`, but the *other* vendor HALs really are in the separate image |

## Continuing from here

```sh
# 1. qemu with the patches applied, statically, for the guest architecture
git clone --depth 1 -b v9.2.0 https://gitlab.com/qemu-project/qemu.git
./patches/apply.sh qemu
cd qemu && ./configure --target-list=aarch64-linux-user --static \
    --disable-system --disable-tools && make -j"$(nproc)"
sudo install -m0755 build/qemu-aarch64 /usr/local/bin/qemu-aarch64-static
sudo update-binfmts --enable qemu-aarch64     # or re-register to the new path

# 2. re-run bring-up
./qandroid image fetch --abi arm64 && ./qandroid rootfs build
sudo ./qandroid boot            # mode=init is the faithful path once ioctls work
```

Expected next failures once binder accepts transactions, in the order they will
appear: `servicemanager` registering (needs `/dev/binder` + the property area),
`zygote64` (needs a populated property area — `init` mode provides it,
`manual` mode does not), then SurfaceFlinger's composer lookup
(`ro.hardware.hwcomposer=qandroid`, see `docs/HWC.md`).

## Note on hosted runners

`binderfs` needed `linux-modules-extra-$(uname -r)` before `modprobe
binder_linux` worked; the first runs had no binder device at all. That is now a
documented prerequisite rather than a mystery, and `qandroid doctor` reports it.
