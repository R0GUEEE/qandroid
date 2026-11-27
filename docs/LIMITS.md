# Limits, and what each one costs

## Performance

TCG is roughly 5–15× slower than native for integer code, worse for
floating point on some paths. Consequences for real apps:

| Phase | Expectation |
|---|---|
| Boot to zygote | 40–90 s on a desktop CPU (real Android: 10–20 s) |
| `pm install` of a 30 MB APK | 20–60 s, dominated by ART's dex2oat under TCG |
| App cold start | 3–8 s |
| Scrolling/UI | usable only with the screencap viewer, at 3–10 fps |
| Native compute (e.g. codec work) | 5–10× slower; hardware codecs do not exist here |

Mitigations that actually help: `-cpu max` (avoids emulating enabled-but-unsupported
features), pre-compiling the APK on a real device (`dex2oat` output can be
copied into `/data/app`), and keeping the guest running (zygote stays warm).

## No GPU

There is no hardware GPU in the guest, and the arm64 userspace ships no driver
for the host's. Two paths:

* SwiftShader inside the guest (`libEGL_swiftshader.so`, if the image has it) —
  correct, CPU-rendered, slow.
* A custom HWC backend that hands buffers to the host compositor — the
  Waydroid model, and the only one that gets near-native UI. Not in v0.1; the
  `screencap` poller is the stopgap.

Anything using Vulkan, hardware MediaCodec, or camera will fail or fall back.

## Architecture coverage

* **arm64-v8a** — yes, this is the whole point.
* **armeabi-v7a only** — no. Needs a second guest (`qemu-arm`) plus an arm32
  userspace and a 32-bit-capable image. Modern images are arm64-only anyway
  (Play requires 64-bit), so most current APKs carry arm64 code.
* **x86/x86_64 libs inside an APK** — deliberately ignored; they would run
  natively but linking them into an arm64 process is not possible.

## Kernel requirements (host)

| Feature | Needed for | Ubuntu 22.04/24.04 | WSL2 |
|---|---|---|---|
| `CONFIG_ANDROID_BINDERFS` | everything | yes | **no** — WSL has no binder; the layer cannot work |
| `CONFIG_ANDROID_BINDER_IPC` | binderfs to exist at all | yes | no |
| `memfd_create`, `F_ADD_SEALS` | Android 12+ shared memory | yes | yes |
| user namespaces + PID ns | `unshare --pid` for init | yes | yes |
| `/dev/ashmem` | Android ≤ 11 images | **no** (removed in 5.18) | no |
| loop devices | only if you mount images instead of `debugfs` | yes | no |

## Filesystem / SELinux

No `selinuxfs` in the guest, so SELinux is effectively permissive. Most things
work; vendor domains log denials that never happen on a device. Anything that
*checks* SELinux mode (`ro.boot.selinux`, `isSELinuxEnabled`) sees whatever the
image's build.prop says and may behave unexpectedly.

Permissions are approximated: without the full `init` uid/domain setup (in
`manual` boot mode) every process is root, which Android is not designed for.
`init` mode is faithful because `init` itself does the `setuid`s.

## Things that will never work here

* **DRM-protected playback** (Widevine L1): needs a hardware TEE.
* **SafetyNet / Play Integrity**: the whole point of those checks is that the
  platform is genuine. A translated, rootful, non-device image fails, and it
  should.
* **Apps with kernel-driver assumptions** (custom `/dev` nodes, raw block
  devices, vendor HALs for real hardware).
* **Anything > 4 GiB RSS per process** on 32-bit host kernels, which is moot on
  x86_64 but worth stating.

## Debugging playbook

| Symptom | First thing to check |
|---|---|
| `exec format error` | `binfmt_misc` registration (`cat /proc/sys/fs/binfmt_misc/qemu-aarch64`) |
| `Unsupported syscall` flood | qemu version; use `-strace` on one process, not the whole guest |
| `init: cannot open /dev/binder` | binderfs mount + `tools/binder-mk` output |
| zygote starts, apps die instantly | property area missing (use `init` mode), or `dalvik.vm.*` unknown to ART |
| surfaceflinger alive, viewer black | composer HAL absent; `screencap` returns a short frame → check `logcat` from inside the guest |
| `pm install` hangs | ART's dex2oat under TCG; give it 2–3 minutes before declaring failure |
