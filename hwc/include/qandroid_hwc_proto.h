/*
 * qandroid_hwc_proto.h - wire protocol between the guest composer HAL and the
 * host display client.  Shared verbatim by both sides; no generated code, no
 * HIDL/AIDL marshalling, no dependency on the Android build system.
 *
 * Transport: AF_UNIX SOCK_SEQPACKET, one message per frame.
 *   guest -> host  QH_OP_FRAME   header + damage rects + SCM_RIGHTS(out_fd)
 *   host  -> guest QH_OP_ACK     {seq} once the frame has been consumed
 *   guest -> host  QH_OP_BYE     on shutdown
 *
 * Backpressure: the guest owns a pool of N buffers and will not reuse a buffer
 * until the host has acked the frame that used it.  That keeps presentation
 * honest instead of dropping or tearing.
 */
#ifndef QANDROID_HWC_PROTO_H
#define QANDROID_HWC_PROTO_H

#include <stdint.h>

#define QH_MAGIC 0x51485743u /* 'QHWC' */
#define QH_MAX_DAMAGE 8
#define QH_POOL 3

enum {
    QH_OP_FRAME = 1,
    QH_OP_ACK = 2,
    QH_OP_BYE = 3,
};

/* Pixel formats: the subset a software composer can actually produce. */
enum {
    QH_FMT_RGBA8888 = 1, /* 4 bytes, byte order R,G,B,A in memory */
    QH_FMT_BGRA8888 = 2,
    QH_FMT_RGB565 = 3,
};

struct qh_rect {
    int32_t x, y, w, h;
};

struct qh_frame {
    uint32_t magic;
    uint32_t op;
    uint32_t seq;
    uint32_t width;
    uint32_t height;
    uint32_t stride;   /* in pixels */
    uint32_t format;   /* QH_FMT_* */
    uint32_t n_damage; /* <= QH_MAX_DAMAGE */
    uint32_t flags;    /* bit0: full-redraw hint (ignore damage) */
    uint32_t _pad;
    struct qh_rect damage[QH_MAX_DAMAGE];
};

struct qh_ack {
    uint32_t magic;
    uint32_t op;
    uint32_t seq;
};

#define QH_SOCK_ENV "QANDROID_HWC_SOCK"
#define QH_SOCK_DEFAULT "/data/local/tmp/qandroid-hwc.sock"

#endif /* QANDROID_HWC_PROTO_H */
