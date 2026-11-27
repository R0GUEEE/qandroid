# Architecture: where each ABI boundary is crossed

Running an arm64 Android APK on x86_64 means crossing six distinct boundaries.
Only two of them are actually about architecture; the rest are the same
problems any Android-on-Linux layer faces, and all of them are solved by
keeping the real Android userspace.

| # | Boundary | Who handles it | Failure symptom if wrong |
|---|---|---|---|
| 1 | **Instruction set** arm64 → x86_64 | `qemu-aarch64-static` (TCG), registered in `binfmt_misc` | `exec format error`, `SIGILL`, or "Unsupported syscall" spam |
| 2 | **Syscall ABI** aarch64 numbers + struct layouts → x86_64 | qemu `linux-user` marshalling | wrong `struct stat`, truncated pointers, `EINVAL` on `ioctl` |
| 3 | **Libc ABI** bionic ↔ glibc | *not crossed* — bionic lives in the guest | if you tried to reimplement it: missing symbol versions, `__sF`, `__libc_init` |
| 4 | **Kernel-facility ABI** binder, ashmem/memfd, futex, namespaces | host kernel; arch independent | `ENODEV` on `/dev/binder`, no property area, zygote hangs |
| 5 | **IPC semantics** binder transaction layout, `hwservicemanager` | guest `libbinder`/`libhwbinder` | services start but `ServiceManager: not found` |
| 6 | **Display** gralloc buffers + composer HAL | *gap* — see LIMITS | surfaceflinger starts, nothing appears |

Boundary 3 is the important one to notice: by keeping an arm64 Android
userspace, it does not have to be crossed at all. That is the entire argument
for user-mode translation over a translator library.

## Boot sequence

```
host (root)                                guest (chroot, PID 1 in a new PID ns)
──────────────────────────────────────────────────────────────────────────────
mount proc / sys / rbind dev
mount -t binder binder dev/binderfs
  binder-mk … binder hwbinder vndbinder
  mount --bind each → dev/binder …
                                            /system/bin/init
                                              ├─ mounts /dev, /dev/pts, /dev/socket
                                              ├─ creates /dev/__properties__ and the
                                              │  property area (shared mmap + futexes)
                                              ├─ parses /system/etc/init/*.rc
                                              ├─ service_manager (AIDL, uid 1000)
                                              ├─ hwservicemanager (HIDL)
                                              ├─ surfaceflinger
                                              ├─ audioserver, media*, netd, storaged
                                              └─ zygote64 → app_process64 --zygote
unshare --pid --mount --fork --mount-proc         │
  chroot ROOTFS /system/bin/sh …                  └─ forked app processes
```

Two host prerequisites drive that layout:

* **PID namespace.** `init` refuses to be anything but PID 1, and it reaps from
  an empty table. `unshare --pid --fork` gives it one. Without this you get a
  zombie storm and `init: cannot set up property area`.
* **Binder devices must exist before init's `.rc` files run.** `init.rc` opens
  `/dev/binder` during `on early-init`; binderfs nodes are created by the host
  (qemu cannot create them, and the guest has no mknod privileges worth giving).

## Rootfs layout

```
ROOTFS/
├── system/     system.img   (extracted with debugfs rdump — no loop, no root)
├── vendor/     vendor.img
├── apex/  -> system/apex     ART runtime, i18n, tzdata live here on Android 10+
├── bin    -> system/bin
├── lib64  -> system/lib64
├── lib    -> system/lib
├── etc    -> system/etc
├── data/       /data/local/tmp, /data/app, /data/property
├── dev/        host bind mounts + binderfs
├── proc/ sys/  host mounts
└── sdcard/ tmp/ mnt/ storage/ metadata/
```

`debugfs -R "rdump / dest"` is deliberate: it extracts an ext4 image as a plain
user, so `rootfs build` never needs root or loop devices. Only `boot` does.

## Why `pm install` instead of `adb`

`adb` speaks a protocol over a USB/TCP transport to `adbd`, which is itself an
Android service that needs the framework up. Inside the guest we already have
root on a chroot — so `qandroid apk install` copies the APK into
`/data/local/tmp` and runs `/system/bin/pm install -r -g`. That exercises
`PackageManagerService`, the real installer, and works while `adbd` is still
being debugged.

## The layers of the repo, mapped to the boundaries

* `tools/lpunpack.py` — boundary 0, "get the userspace out of the shipped image"
  (dynamic partitions: geometry at 4096, slot 0 at 8192, extents as
  `(num_sectors, target_type, target_data, target_source)`).
* `tools/apk.py` — APK side: which ABI does this app even have, and what does
  its binary manifest say.
* `patches/apply.sh` — boundary 1/2 fixups; see `patches/README.md` for the
  full audit of what qemu already handles versus what needs the one patch.
* `scripts/30-boot.sh` + `scripts/guest-boot.sh` — boundary 4 and the start of 5.
* `shim/abshim.c` — for the *other* use case: an arm64 ELF pulled out of an APK
  and run against the host runtime. It is a symbol shim, not a translation
  layer, and it cannot replace boundary 3 for anything non-trivial.
