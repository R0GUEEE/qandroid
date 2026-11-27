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
| 8 | qemu built from source with the ioctl patch, invoked directly | **`servicemanager` runs 40 s until the time-box kills it (exit 124)** — no `Binder driver could not be opened`, no abort |

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

## The ioctl patch is confirmed

Run 8 launches a single guest process under a qemu built from source with
`patches/apply.sh` applied, invoked explicitly rather than through binfmt:

```
direct mode: /system/bin/servicemanager under /usr/local/bin/qemu-aarch64-static
boot exit: 124            # timeout killed it after 40 s, it never exited
```

Compare with every earlier run, where the same binary aborted in under a second
with `Binder driver '/dev/binder' could not be opened. Terminating: Binder ioctl
to obtain version failed`. A service manager that sits there serving is what
"binder works" looks like from the outside.

Why direct invocation: making *binfmt* use the patched binary is what a full
boot needs (children exec too), but it cannot be done safely on a hosted runner —
see the next section.

## What each binfmt subtlety cost

Getting a patched qemu into the kernel's path took four attempts, each teaching
something worth writing down:

1. Installing over `/usr/bin/qemu-aarch64-static` changed nothing: Ubuntu
   registers **`/usr/libexec/qemu-binfmt/aarch64-binfmt-P`** as the interpreter.
2. Copying to the right path still changed nothing: the entry has the **F
   (fix-binary)** flag, so the kernel keeps the inode it opened at registration.
3. Writing `-1` to the entry to release that cache **removed the entry**, and
   `update-binfmts` does not manage it (`not in database of installed binary
   formats`).
4. Registering a fresh entry caught two more: `printf '%s'` leaves `\x7f` as
   four characters (entry matches nothing), and `printf` **stops at the first
   NUL**, which registers a truncated magic that matches *every* ELF — after
   that, the runner's own `bash` and `node` failed with `Exec format error`
   twice in a row.

Conclusion: own the entry from the start, write the bytes from a program that
handles NULs (Python), and register it in a job that does not also install the
distro `qemu-user-static`. Or run on real hardware, where `install` +
`update-binfmts` just works.

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
