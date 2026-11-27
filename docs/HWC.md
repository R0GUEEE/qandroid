# HWC backend: how SurfaceFlinger reaches the host window

## Why a composer HAL and not surface capture

`screencap` round-trips every frame through a PNG encode/decode and a shell
process: 3–10 fps and no input path. The actual compositor interface in Android
is HWC2, and SurfaceFlinger already drives it at vsync rate. Implementing the
HWC means we get the frames SurfaceFlinger is already producing, in the format
it is already producing them, with damage rectangles and fences intact.

```
app → SurfaceFlinger ──HWC2──► hwcomposer.qandroid.so ──socket+SCM_RIGHTS──► hwc_client
                              (guest, arm64, under TCG)                     (host, x86_64)
                                                                                  │
                                                                          SDL window
                                                                                  │
app ← InputReader ← /dev/input/eventN ← uinput device ◄───────────────────────────┘
```

## The two halves

**Guest — `hwc/src/hwcomposer_qandroid.c`** (`hwcomposer.qandroid.so`)
implements `hwc2_device_t`: layer creation, buffer/crop/frame/z/blend/alpha,
`validateDisplay` (accepts every layer as `DEVICE`), `presentDisplay` (CPU
composite into a pooled output buffer, send, wait for ack), `getReleaseFences`,
`setClientTarget`, one display config, power mode.

Layers are mapped with gralloc0 `lock()`; stride comes from `gralloc_priv.h`'s
`private_handle_t` when the vendor header is visible, otherwise the crop width.
Compositing is source-over with premultiplied alpha and nearest-neighbour
scaling. Transforms are refused (`HWC2_ERROR_UNSUPPORTED`), which makes
SurfaceFlinger composite those layers itself into the client target — which we
then blit. Correct, just not free.

**Host — `frontend/hwc_client.c`**
listens on the unix socket, mmaps each received buffer, uploads it as an SDL
texture, acks, and turns mouse input into a uinput virtual touchscreen
(`INPUT_PROP_DIRECT`, ABS_X/ABS_Y, BTN_TOUCH). No libinput, no root on the guest
side, no injected keystrokes: InputReader sees a normal evdev device.

## Wire protocol (`hwc/include/qandroid_hwc_proto.h`)

`AF_UNIX` / `SOCK_SEQPACKET`, one message per frame, fd passed in `SCM_RIGHTS`:

| message | direction | payload |
|---|---|---|
| `QH_OP_FRAME` | guest → host | `struct qh_frame` (168 B: geometry, format, up to 8 damage rects) + one fd |
| `QH_OP_ACK` | host → guest | `struct qh_ack` (12 B: seq) |
| `QH_OP_BYE` | guest → host | header only, on composer teardown |

Backpressure is a 3-buffer pool: a buffer is not reused until its seq is acked,
and the guest waits up to 200 ms for the ack inside `presentDisplay`. If the
host is gone entirely, frames are dropped and the guest keeps running — a dead
display client must never stall SurfaceFlinger.

## Building

**Syntax check** (no AOSP tree, no arm64 toolchain):

```sh
make -C hwc check
```

**Guest module** (any aarch64 cross compiler; shim headers):

```sh
make -C hwc AARCH64_CC=aarch64-linux-gnu-gcc
```

**Shipping build** — inside a LineageOS/AOSP tree matching the image tag:

```sh
cp -r hwc hardware/qandroid/hwc
m hwcomposer.qandroid
```

That build uses the platform's `<hardware/hwcomposer2.h>`, which is what makes
the `HWC2_FUNCTION_*` descriptor numbers correct. The vendored header exists
only so the module can be compile-checked standalone; its values follow Android
13 and must not be shipped.

**Host client**:

```sh
cc -O2 -o hwc_client frontend/hwc_client.c hwc/include/qandroid_hwc_proto.h \
   $(pkg-config --cflags --libs sdl2)
```

## Wiring it into the guest

```sh
install -m0644 hwcomposer.qandroid.so $ROOTFS/vendor/lib64/hw/
# make the composer service find it:
echo 'ro.hardware.hwcomposer=qandroid' >> $ROOTFS/system/build.prop
```

Two integration details that decide whether this works at all:

1. **The passthrough composer service must exist.** SurfaceFlinger talks to
   `android.hardware.graphics.composer@2.x` / `@3`, which is a thin service that
   calls `hw_get_module(HWC_HARDWARE_MODULE_ID)` and `hwc2_open()`. LineageOS
   images ship `android.hardware.graphics.composer@2.1-service`; that binary
   loads whatever `ro.hardware.hwcomposer` names. If your image ships no such
   service, it must be added (or built from
   `hardware/interfaces/graphics/composer/2.1/default`).
2. **Socket path.** The guest composer connects to `$QANDROID_HWC_SOCK`
   (default `/data/local/tmp/qandroid-hwc.sock`). Start `hwc_client` with the
   same path, mapped into the chroot. Order does not matter: the guest retries
   the connect on the next frame and logs one warning.

## Known limits of this backend

| Limit | Effect | Workaround |
|---|---|---|
| No transformation support | rotated layers are client-composited by SF | acceptable: SF handles it, just costs its own GPU path... which is also CPU here |
| RGBA_8888 only | YUV/video layers are not decoded by us | SF falls back to client composition for them |
| Nearest-neighbour scaling | slightly soft when the host window is scaled | use `--scale 1` or a matching window size |
| Present fence is a `sw_sync` fence | exact per-frame sync | if `/dev/sw_sync` is missing we return `-1`, which SF tolerates |
| One display, one config | no HDMI/multi-display, no refresh switching | fine for a single window |
| No HDR, no wide colour | colours are 8-bit sRGB | inherent to the software path |
| CPU compositing cost | one full-screen memcpy + blend per frame under TCG | 1280×720 is the practical ceiling; prefer 960×540 for heavy UIs |

## Performance expectation

The composite itself is cheap compared to translation: at 1280×720 a single
full-screen blit is ~3.7 MB, and TCG runs that loop maybe 6–10× slower than
native. Budget roughly 8–15 ms per frame on a modern desktop for one full-screen
layer, so 30–60 fps is realistic for simple UIs; the *producer* side (app
rendering under TCG) is what limits heavier screens.

## Debugging

* Guest: `logcat -s qandroid-hwc` — the module logs connect failures, mapping
  failures and ack timeouts.
* No window content but SF alive? Run the client first, then `setprop
  debug.sf.nobootanimation 1` and check that `dumpsys SurfaceFlinger --dispsync`
  shows a display with our config.
* Input works but coordinates are off? The uinput device geometry is taken from
  the first frame if it arrives after the client starts; otherwise it defaults to
  1280×720. `hwc_client` recreates the device when the frame size changes.
