# Syscall / ABI audit: what actually needs fixing to run arm64 Android on x86_64

Everything here was reasoned from the ABI surface the arm64 Android userspace
uses (`bionic`, `libcutils`, `libhwbinder`, `ART`, `surfaceflinger`). The list
is split into three buckets: handled by the host kernel, handed by qemu-user,
and needs help.

## 1. Handled by the host kernel (arch independent, no work)

| Facility | Why it matters | Note |
|---|---|---|
| `binderfs` (`CONFIG_ANDROID_BINDERFS`) | the IPC fabric of the whole framework | nodes are created via `BINDER_CTL_ADD`; ioctl numbers are `_IOW('b', …)` and arch neutral, so qemu passes them straight through |
| `memfd_create` | `ASharedMemory`, gralloc buffers, ART's `memfd` JIT cache (Android 12+) | sealing (`F_ADD_SEALS`) passes through |
| `/dev/ashmem` | `ASharedMemory` on Android ≤ 11 | removed from staging in Linux 5.18; absent on current Ubuntu. Use an Android 12+ image, or a `/dev/ashmem`-over-FUSE shim |
| `futex`, `futex_waitv`, robust lists | ART monitors, libc mutexes | host kernel implements them; qemu translates |
| `epoll`, `eventfd`, `timerfd`, `signalfd`, `inotify` | every service loop | |
| namespaces, `unshare`, `pivot_root` | guest `init` needing PID 1 + its own mounts |  |
| `statx` | bionic's `stat` family on Android 12+ | kernel ≥ 4.11 |

## 2. Handled by qemu-user (aarch64→x86_64 TCG)

* full arm64 instruction set including LSE atomics, which bionic's `memcpy`
  family and ART switch to at runtime when `HWCAP_ATOMICS` is set. Start qemu
  with `-cpu max` so the advertised HWCAP matches what TCG can execute.
* 64-bit `time_t`, `struct stat`, `iovec`, `msghdr` marshalling between the
  aarch64 guest layout and the x86_64 host layout.
* `clone`/`clone3`, `set_robust_list`, `rseq` are all implemented; ART's GC
  barriers and `art::Thread` startup work.

## 3. Needs the patch in `apply.sh`

`prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, addr, len, name)` — `0x53564d41`.
Used by scudo, jemalloc and ART to name anonymous mappings. `linux-user` has no
mapping-name registry, so it answers `EINVAL`, and some bionic builds log
`art: Failed to name VMA` for every allocation. The patch answers success.
No guest-visible behaviour depends on it.

## 4. Known rough edges that are *not* fixed by patching qemu

* **Zygote's `fork()` + shared-zygote VM**: works, but every app process pays
  TCG translation cost, so cold app launch is 2–4× slower than on real ARM.
* **JIT pages**: ART's JIT allocates `PROT_EXEC` memory; qemu-user maps exec
  pages fine, but `mprotect(PROT_EXEC)` on a TCG-translated region can flush
  translated blocks repeatedly, which is the main throughput tax.
* **32-bit APKs** (`armeabi-v7a`) need a second guest (`qemu-arm`) and an
  arm32 Android userspace; out of scope for v0.1.
* **`ashmem`-only images** (Android ≤ 11) require the FUSE shim or a kernel
  with the staging driver.
* **GPU**: the guest has no hardware GPU. SurfaceFlinger needs either the
  image's SwiftShader (`libEGL_swiftshader.so`) plus a composer HAL that
  accepts software buffers, or a custom HWC backend. v0.1 ships a
  `screencap`-polling viewer as the display fallback and treats a real HWC as
  the next milestone.
* **SELinux**: there is no `selinuxfs` in this setup, so the guest runs with
  SELinux effectively off. `libselinux` degrades to permissive; some vendor
  domains will log denials but continue.
