/*
 * hwcomposer_qandroid - a software HWC2 composer module for qandroid.
 *
 * SurfaceFlinger (through the image's HIDL/AIDL composer service, which is a
 * passthrough to hw_get_module("hwcomposer")) hands us layers.  We composite
 * them on the CPU into a memfd-backed output buffer and ship that buffer, plus
 * damage, to the host display client over a unix socket (fd passed via
 * SCM_RIGHTS).  The host blits it into a window and injects input through a
 * uinput device, so SurfaceFlinger keeps using its normal InputReader.
 *
 *   rootfs/vendor/lib64/hw/hwcomposer.qandroid.so
 *   setprop ro.hardware.hwcomposer qandroid
 *
 * Build inside an AOSP tree matching the image tag (gets real hwcomposer2.h and
 * libui); see hwc/Makefile for both build modes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define LOG_TAG "qandroid-hwc"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include) && __has_include(<hardware/hwcomposer2.h>)
#include <hardware/hwcomposer2.h>
#include <hardware/hardware.h>
#include <android/log.h>
#define QH_HAVE_PLATFORM_HEADERS 1
#else
#include "hwc2_shim.h"
#include "qandroid_hw.h"
#define QH_HAVE_PLATFORM_HEADERS 0
#ifndef __android_log_vprint
/* syntax-check build only */
#include <stdio.h>
static int qh_stub_logv(const char *fmt, va_list ap)
{
    (void)vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return 0;
}
#define __android_log_vprint(p, t, f, a) qh_stub_logv(f, a)
#endif
#endif

/* The gralloc private handle carries the real stride; when we cannot see it
 * (no vendor header), fall back to the crop width and say so once. */
#if defined(__has_include) && __has_include(<gralloc_priv.h>)
#include <gralloc_priv.h>
#define QH_HAVE_GRALLOC_PRIV 1
#else
#define QH_HAVE_GRALLOC_PRIV 0
#endif

#include "qandroid_hwc_proto.h"

#define QH_MAX_LAYERS 32
#define QH_DEFAULT_W 1280
#define QH_DEFAULT_H 720
#define QH_ACK_TIMEOUT_MS 200

#ifndef HWC2_PFN_SET_LAYER_DATASPACE
/* descriptor exists from HWC2 API 1.1 on; we accept and ignore it */
#endif

struct qh_layer {
    int in_use;
    buffer_handle_t buffer;
    struct hwc_rect frame;
    struct hwc_frect crop;
    int32_t blend;
    int32_t composition;
    float alpha;
    uint32_t z;
    int changed;
};

struct qh_outbuf {
    int fd;
    void *addr;
    size_t size;
    uint32_t seq;
    int busy;
};

struct qh_ctx {
    hwc2_device_t device;
    gralloc_module_t const *gralloc;

    uint32_t width, height, stride;
    uint32_t refresh_hz;

    struct qh_layer layers[QH_MAX_LAYERS];
    hwc2_layer_t stack[QH_MAX_LAYERS];
    uint32_t stack_len;

    int sock;
    char sock_path[256];
    struct qh_outbuf pool[QH_POOL];
    uint32_t next_seq;
    int full_redraw;

    int power_on;
    int validated;

    /* pending release fences, one per layer presented last frame */
    int rel_fence[QH_MAX_LAYERS];
    hwc2_layer_t rel_layer[QH_MAX_LAYERS];
    int rel_count;

    int sync_timeline_fd;
    uint32_t sync_inc;
};

static struct qh_ctx g_ctx;

/* ------------------------------------------------------------------ util */
static void qh_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void qh_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(5 /* WARN */, LOG_TAG, fmt, ap);
    va_end(ap);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- sync fences -------------------------------------------------------- */
/* Fence creation without <linux/sync_file.h> dependency: sw_sync ioctls are
 * stable _IOW('s', 1|2, ...), so we call them directly. */
#define QH_SYNC_IOC_MAGIC 's'
struct qh_sync_fence_info { char name[32]; int32_t status; uint32_t flags; };
#define QH_SYNC_IOC_CREATE_FENCE _IOWR(QH_SYNC_IOC_MAGIC, 0, struct qh_sync_fence_info)
#define QH_SYNC_IOC_INC _IOW(QH_SYNC_IOC_MAGIC, 1, int32_t)

static int qh_fence_from_timeline(struct qh_ctx *c)
{
    struct qh_sync_fence_info info;
    memset(&info, 0, sizeof info);
    strcpy(info.name, "qandroid-hwc");
    if (!c || c->sync_timeline_fd < 0)
        return -1;
    int fd = ioctl(c->sync_timeline_fd, QH_SYNC_IOC_CREATE_FENCE, &info);
    return fd;
}

static void qh_fence_signal(struct qh_ctx *c)
{
    int32_t inc = 1;
    if (c && c->sync_timeline_fd >= 0)
        ioctl(c->sync_timeline_fd, QH_SYNC_IOC_INC, &inc);
}

static void qh_wait_fence(int fd)
{
    if (fd < 0)
        return;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    /* sw_sync fences become readable when signaled */
    poll(&p, 1, 1000);
}

static int qh_open_timeline(struct qh_ctx *c)
{
    c->sync_timeline_fd = open("/dev/sw_sync", O_RDWR | O_CLOEXEC);
    if (c->sync_timeline_fd < 0)
        qh_log("no /dev/sw_sync: present fences will be -1 (SurfaceFlinger tolerates it)");
    return c->sync_timeline_fd;
}

/* ---- output pool -------------------------------------------------------- */
static int qh_pool_init(struct qh_ctx *c)
{
    size_t sz = (size_t)c->stride * c->height * 4;
    for (int i = 0; i < QH_POOL; i++) {
        int fd = memfd_create("qandroid-hwc-out", MFD_CLOEXEC);
        if (fd < 0)
            return -1;
        if (ftruncate(fd, sz)) {
            close(fd);
            return -1;
        }
        c->pool[i].addr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (c->pool[i].addr == MAP_FAILED) {
            close(fd);
            return -1;
        }
        c->pool[i].fd = fd;
        c->pool[i].size = sz;
    }
    return 0;
}

static struct qh_outbuf *qh_pool_get(struct qh_ctx *c)
{
    for (int i = 0; i < QH_POOL; i++) {
        if (!c->pool[i].busy)
            return &c->pool[i];
    }
    /* everything in flight: wait for any ack, then retry */
    qh_log("all %d output buffers in flight", QH_POOL);
    return NULL;
}

/* ---- socket ------------------------------------------------------------- */
static int qh_connect(struct qh_ctx *c)
{
    const char *path = getenv(QH_SOCK_ENV);
    snprintf(c->sock_path, sizeof c->sock_path, "%s", path ? path : QH_SOCK_DEFAULT);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", c->sock_path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        qh_log("connect(%s) failed: %s - frames will be dropped until a client appears",
               c->sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    c->sock = fd;
    qh_log("connected to display client at %s", c->sock_path);
    return 0;
}

static int qh_send_frame(struct qh_ctx *c, struct qh_outbuf *buf, uint32_t n_damage,
                         const struct qh_rect *damage)
{
    if (c->sock < 0 && qh_connect(c))
        return -1;

    struct qh_frame f;
    memset(&f, 0, sizeof f);
    f.magic = QH_MAGIC;
    f.op = QH_OP_FRAME;
    f.seq = ++c->next_seq;
    f.width = c->width;
    f.height = c->height;
    f.stride = c->stride;
    f.format = QH_FMT_RGBA8888;
    f.n_damage = n_damage > QH_MAX_DAMAGE ? QH_MAX_DAMAGE : n_damage;
    f.flags = c->full_redraw ? 1u : 0u;
    c->full_redraw = 0;
    for (uint32_t i = 0; i < f.n_damage; i++)
        f.damage[i] = damage[i];

    struct iovec iov = {.iov_base = &f, .iov_len = sizeof f};
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof ctrl);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &buf->fd, sizeof(int));

    if (sendmsg(c->sock, &mh, MSG_NOSIGNAL) < 0) {
        qh_log("sendmsg: %s - dropping client", strerror(errno));
        close(c->sock);
        c->sock = -1;
        return -1;
    }
    buf->busy = 1;
    buf->seq = f.seq;

    /* wait for the ack so the buffer is not reused while the host reads it */
    int64_t deadline = now_ms() + QH_ACK_TIMEOUT_MS;
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            qh_log("ack timeout, releasing buffer anyway (seq %u)", f.seq);
            buf->busy = 0;
            return 0;
        }
        struct pollfd p = {.fd = c->sock, .events = POLLIN};
        int r = poll(&p, 1, (int)left);
        if (r <= 0)
            continue;
        struct qh_ack ack;
        ssize_t n = recv(c->sock, &ack, sizeof ack, MSG_DONTWAIT);
        if (n == (ssize_t)sizeof ack && ack.magic == QH_MAGIC && ack.op == QH_OP_ACK) {
            for (int i = 0; i < QH_POOL; i++)
                if (c->pool[i].seq == ack.seq)
                    c->pool[i].busy = 0;
        }
    }
}

/* ---- blitting ----------------------------------------------------------- */
struct qh_srcview {
    const uint8_t *base;
    uint32_t stride; /* pixels */
};

/* Nearest-neighbour scale + source-over blend of one layer into dst. */
static void qh_blit(uint8_t *dst, uint32_t dst_stride, uint32_t dst_w, uint32_t dst_h,
                    const struct qh_srcview *src, struct hwc_rect frame,
                    struct hwc_frect crop, float alpha, int premultiplied)
{
    if (!src || !src->base)
        return;
    int32_t x0 = frame.left < 0 ? 0 : frame.left;
    int32_t y0 = frame.top < 0 ? 0 : frame.top;
    int32_t x1 = frame.right > (int32_t)dst_w ? (int32_t)dst_w : frame.right;
    int32_t y1 = frame.bottom > (int32_t)dst_h ? (int32_t)dst_h : frame.bottom;
    int32_t fw = frame.right - frame.left;
    int32_t fh = frame.bottom - frame.top;
    if (fw <= 0 || fh <= 0 || x1 <= x0 || y1 <= y0)
        return;

    uint32_t sw = (uint32_t)(crop.right - crop.left);
    uint32_t sh = (uint32_t)(crop.bottom - crop.top);
    if (!sw || !sh)
        return;

    for (int32_t y = y0; y < y1; y++) {
        uint32_t sy = (uint32_t)crop.top + (uint32_t)(((y - frame.top) * (int64_t)sh) / fh);
        if (sy >= sh)
            sy = sh - 1;
        const uint8_t *srow = src->base + (size_t)sy * src->stride * 4;
        uint8_t *drow = dst + (size_t)y * dst_stride * 4;
        for (int32_t x = x0; x < x1; x++) {
            uint32_t sx = (uint32_t)crop.left + (uint32_t)(((x - frame.left) * (int64_t)sw) / fw);
            if (sx >= sw)
                sx = sw - 1;
            const uint8_t *s = srow + (size_t)sx * 4;
            uint8_t *d = drow + (size_t)x * 4;
            if (premultiplied) {
                if (s[3] == 255) {
                    memcpy(d, s, 4);
                } else if (s[3]) {
                    uint32_t a = (uint32_t)((s[3] * alpha) + 0.5f);
                    uint32_t ia = 255 - a;
                    d[0] = (uint8_t)(s[0] + (d[0] * ia) / 255);
                    d[1] = (uint8_t)(s[1] + (d[1] * ia) / 255);
                    d[2] = (uint8_t)(s[2] + (d[2] * ia) / 255);
                    d[3] = 255;
                }
            } else {
                memcpy(d, s, 4);
                d[3] = 255;
            }
        }
    }
}

static uint32_t qh_stride_of(buffer_handle_t handle, uint32_t fallback)
{
#if QH_HAVE_GRALLOC_PRIV
    const private_handle_t *h = (const private_handle_t *)handle;
    if (h && h->stride >= h->width)
        return (uint32_t)h->stride;
#else
    (void)handle;
#endif
    return fallback;
}

static int qh_lock_handle(struct qh_ctx *c, buffer_handle_t handle, void **addr, uint32_t *stride)
{
    if (!handle)
        return -1;
    if (!c->gralloc) {
        static int warned;
        if (!warned++)
            qh_log("no gralloc module loaded: cannot map layer buffers");
        return -1;
    }
    /* gralloc0 lock: the only mapping API that is stable across every image we
     * target, and it needs no C++ runtime on the module side */
    int usage = 0x00000003 /* GRALLOC_USAGE_SW_READ_OFTEN */;
    int r = c->gralloc->lock(c->gralloc, handle, usage, 0, 0, 0, 0, addr);
    if (r == 0)
        *stride = qh_stride_of(handle, c->stride);
    return r;
}

static void qh_unlock_handle(struct qh_ctx *c, buffer_handle_t handle)
{
    if (c->gralloc && handle)
        c->gralloc->unlock(c->gralloc, handle);
}

/* ---------------------------------------------------------------- hwc2 API */
static int qh_create_layer(hwc2_device_t *dev, hwc2_display_t display, hwc2_layer_t *out)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    for (int i = 0; i < QH_MAX_LAYERS; i++) {
        if (!c->layers[i].in_use) {
            memset(&c->layers[i], 0, sizeof c->layers[i]);
            c->layers[i].in_use = 1;
            c->layers[i].blend = HWC2_BLEND_MODE_PREMULTIPLIED;
            c->layers[i].alpha = 1.0f;
            c->layers[i].composition = HWC2_COMPOSITION_DEVICE;
            *out = (hwc2_layer_t)(i + 1);
            return HWC2_ERROR_NONE;
        }
    }
    return HWC2_ERROR_NO_RESOURCES;
}

static struct qh_layer *qh_layer(struct qh_ctx *c, hwc2_layer_t id)
{
    if (id == 0 || id > QH_MAX_LAYERS)
        return NULL;
    struct qh_layer *l = &c->layers[id - 1];
    return l->in_use ? l : NULL;
}

static int qh_destroy_layer(hwc2_device_t *dev, hwc2_layer_t id)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    l->in_use = 0;
    return HWC2_ERROR_NONE;
}

static int qh_set_layer_buffer(hwc2_device_t *dev, hwc2_layer_t id, buffer_handle_t buf, int fence)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    qh_wait_fence(fence);
    if (fence >= 0)
        close(fence);
    l->buffer = buf;
    l->changed = 1;
    return HWC2_ERROR_NONE;
}

static int qh_set_source_crop(hwc2_device_t *dev, hwc2_layer_t id, const struct hwc_frect *crop)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    if (crop) {
        l->crop = *crop;
        l->changed = 1;
    }
    return HWC2_ERROR_NONE;
}

static int qh_set_display_frame(hwc2_device_t *dev, hwc2_layer_t id, const struct hwc_rect *frame)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    if (frame) {
        l->frame = *frame;
        l->changed = 1;
    }
    return HWC2_ERROR_NONE;
}

static int qh_set_composition_type(hwc2_device_t *dev, hwc2_layer_t id, int32_t type)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    l->composition = type;
    l->changed = 1;
    return HWC2_ERROR_NONE;
}

static int qh_set_blend_mode(hwc2_device_t *dev, hwc2_layer_t id, int32_t mode)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    l->blend = mode;
    l->changed = 1;
    return HWC2_ERROR_NONE;
}

static int qh_set_plane_alpha(hwc2_device_t *dev, hwc2_layer_t id, float alpha)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    l->alpha = alpha;
    l->changed = 1;
    return HWC2_ERROR_NONE;
}

static int qh_set_transform(hwc2_device_t *dev, hwc2_layer_t id, int32_t transform)
{
    (void)dev;
    (void)id;
    /* rotation/flip not implemented: accept only identity so SF composites
       such layers itself into the client target, which we do blit */
    return transform == HWC2_TRANSFORM_NORMAL ? HWC2_ERROR_NONE : HWC2_ERROR_UNSUPPORTED;
}

static int qh_set_z(hwc2_device_t *dev, hwc2_layer_t id, uint32_t z)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    struct qh_layer *l = qh_layer(c, id);
    if (!l)
        return HWC2_ERROR_BAD_LAYER;
    l->z = z;
    return HWC2_ERROR_NONE;
}

static int qh_set_visible_region(hwc2_device_t *dev, hwc2_layer_t id, const struct hwc_region *r)
{
    (void)dev;
    (void)id;
    (void)r;
    return HWC2_ERROR_NONE; /* always render the full frame */
}

static int qh_set_layer_stack(hwc2_device_t *dev, hwc2_display_t display, uint32_t n,
                              const hwc2_layer_t *layers)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    if (n > QH_MAX_LAYERS)
        n = QH_MAX_LAYERS;
    for (uint32_t i = 0; i < n; i++) {
        if (!qh_layer(c, layers[i]))
            return HWC2_ERROR_BAD_LAYER;
        c->stack[i] = layers[i];
    }
    c->stack_len = n;
    return HWC2_ERROR_NONE;
}

static int qh_validate_display(hwc2_device_t *dev, hwc2_display_t display,
                               uint32_t *out_num_types, uint32_t *out_num_requests)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    *out_num_types = 0; /* we accept every layer as DEVICE */
    *out_num_requests = 0;
    c->validated = 1;
    return HWC2_ERROR_NONE;
}

static int qh_accept_changes(hwc2_device_t *dev, hwc2_display_t display)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    if (!c->validated)
        return HWC2_ERROR_NOT_VALIDATED;
    c->validated = 0;
    return HWC2_ERROR_NONE;
}

static int qh_present(hwc2_device_t *dev, hwc2_display_t display, int *out_fence)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;

    *out_fence = qh_fence_from_timeline(c);

    struct qh_outbuf *buf = qh_pool_get(c);
    if (!buf) {
        /* no free buffer: still report success, SF will retry next vsync */
        qh_fence_signal(c);
        return HWC2_ERROR_NONE;
    }
    uint8_t *dst = (uint8_t *)buf->addr;
    memset(dst, 0, buf->size);

    /* stable order by z, then layer id */
    struct qh_layer *order[QH_MAX_LAYERS];
    int n = 0;
    for (uint32_t i = 0; i < c->stack_len && n < QH_MAX_LAYERS; i++) {
        struct qh_layer *l = qh_layer(c, c->stack[i]);
        if (!l || !l->buffer)
            continue;
        order[n++] = l;
    }
    for (int i = 1; i < n; i++) { /* insertion sort, n <= 32 */
        struct qh_layer *key = order[i];
        int j = i - 1;
        while (j >= 0 && (order[j]->z > key->z ||
                          (order[j]->z == key->z && order[j] < key))) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    struct qh_rect damage[QH_MAX_DAMAGE];
    uint32_t ndamage = 0;
    int any_change = c->full_redraw;

    for (int i = 0; i < n; i++) {
        struct qh_layer *l = order[i];
        void *addr = NULL;
        uint32_t stride = 0;
        if (qh_lock_handle(c, l->buffer, &addr, &stride) != 0) {
            qh_log("cannot map layer buffer, skipped");
            continue;
        }
        struct qh_srcview src = {.base = (const uint8_t *)addr, .stride = stride};
        struct hwc_frect crop = l->crop;
        if (crop.right <= crop.left || crop.bottom <= crop.top) {
            crop.left = 0;
            crop.top = 0;
            crop.right = (float)c->width;
            crop.bottom = (float)c->height;
        }
        qh_blit(dst, c->stride, c->width, c->height, &src, l->frame, crop, l->alpha,
                l->blend == HWC2_BLEND_MODE_PREMULTIPLIED);
        qh_unlock_handle(c, l->buffer);

        if (l->changed && ndamage < QH_MAX_DAMAGE) {
            damage[ndamage].x = l->frame.left;
            damage[ndamage].y = l->frame.top;
            damage[ndamage].w = l->frame.right - l->frame.left;
            damage[ndamage].h = l->frame.bottom - l->frame.top;
            ndamage++;
            any_change = 1;
        }
        l->changed = 0;
    }
    if (!any_change)
        c->full_redraw = 0;

    qh_send_frame(c, buf, ndamage, damage);
    qh_fence_signal(c);

    /* every presented layer gets a release fence, already signaled */
    c->rel_count = 0;
    for (int i = 0; i < n && c->rel_count < QH_MAX_LAYERS; i++) {
        c->rel_fence[c->rel_count] = -1;
        c->rel_layer[c->rel_count] = (hwc2_layer_t)(order[i] - c->layers + 1);
        c->rel_count++;
    }
    return HWC2_ERROR_NONE;
}

static int qh_get_release_fences(hwc2_device_t *dev, hwc2_display_t display,
                                 uint32_t *out_num, int *out_fences, hwc2_layer_t *out_layers)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    for (int i = 0; i < c->rel_count; i++) {
        out_layers[i] = c->rel_layer[i];
        out_fences[i] = c->rel_fence[i];
    }
    *out_num = (uint32_t)c->rel_count;
    c->rel_count = 0;
    return HWC2_ERROR_NONE;
}

static int qh_set_client_target(hwc2_device_t *dev, hwc2_display_t display,
                                buffer_handle_t target, int fence)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    qh_wait_fence(fence);
    if (fence >= 0)
        close(fence);
    /* SurfaceFlinger composited everything into this buffer; present it as a
       full-screen layer 0 so the normal path draws it */
    struct qh_layer *l = &c->layers[QH_MAX_LAYERS - 1];
    memset(l, 0, sizeof *l);
    l->in_use = 1;
    l->buffer = target;
    l->frame.left = 0;
    l->frame.top = 0;
    l->frame.right = (int32_t)c->width;
    l->frame.bottom = (int32_t)c->height;
    l->crop.left = 0;
    l->crop.top = 0;
    l->crop.right = (float)c->width;
    l->crop.bottom = (float)c->height;
    l->blend = HWC2_BLEND_MODE_NONE;
    l->alpha = 1.0f;
    l->z = 0;
    l->changed = 1;
    c->full_redraw = 1;
    return HWC2_ERROR_NONE;
}

static int qh_set_active_config(hwc2_device_t *dev, hwc2_display_t display, hwc2_config_t cfg)
{
    (void)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    return cfg == 0 ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_CONFIG;
}

static hwc2_config_t qh_get_active_config(hwc2_device_t *dev, hwc2_display_t display)
{
    (void)dev;
    return display == 0 ? 0 : (hwc2_config_t)-1;
}

static int qh_set_power_mode(hwc2_device_t *dev, hwc2_display_t display, int32_t mode)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    c->power_on = (mode != HWC2_POWER_MODE_OFF);
    c->full_redraw = 1;
    return HWC2_ERROR_NONE;
}

static int qh_get_display_attribute(hwc2_device_t *dev, hwc2_display_t display, hwc2_config_t cfg,
                                    int32_t attr, int32_t *out)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (display != 0 || cfg != 0)
        return HWC2_ERROR_BAD_CONFIG;
    switch (attr) {
    case HWC2_ATTRIBUTE_WIDTH: *out = (int32_t)c->width; return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_HEIGHT: *out = (int32_t)c->height; return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_VSYNC_PERIOD: *out = (int32_t)(1000000000ull / c->refresh_hz); return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_DPI_X: *out = 160 * 25 / 4; return HWC2_ERROR_NONE;
    case HWC2_ATTRIBUTE_DPI_Y: *out = 160 * 25 / 4; return HWC2_ERROR_NONE;
    default: return HWC2_ERROR_BAD_PARAMETER;
    }
}

static int qh_get_display_configs(hwc2_device_t *dev, hwc2_display_t display, uint32_t *out_num,
                                  hwc2_config_t *out_cfgs)
{
    (void)dev;
    if (display != 0)
        return HWC2_ERROR_BAD_DISPLAY;
    *out_num = 1;
    if (out_cfgs)
        out_cfgs[0] = 0;
    return HWC2_ERROR_NONE;
}

static void qh_get_capabilities(hwc2_device_t *dev, uint32_t *out_count, int32_t *out_caps)
{
    (void)dev;
    *out_count = 1;
    if (out_caps)
        out_caps[0] = HWC2_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
}

static int32_t qh_get_function(hwc2_device_t *dev, int32_t desc, int32_t *out_version,
                               hwc2_function_pointer_t *out_fn)
{
    (void)dev;
    *out_version = HWC2_DEVICE_API_VERSION_1_0;
    switch (desc) {
    case HWC2_FUNCTION_CREATE_LAYER: *out_fn = (hwc2_function_pointer_t)qh_create_layer; break;
    case HWC2_FUNCTION_DESTROY_LAYER: *out_fn = (hwc2_function_pointer_t)qh_destroy_layer; break;
    case HWC2_FUNCTION_SET_LAYER_BUFFER: *out_fn = (hwc2_function_pointer_t)qh_set_layer_buffer; break;
    case HWC2_FUNCTION_SET_LAYER_SOURCE_CROP: *out_fn = (hwc2_function_pointer_t)qh_set_source_crop; break;
    case HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME: *out_fn = (hwc2_function_pointer_t)qh_set_display_frame; break;
    case HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE: *out_fn = (hwc2_function_pointer_t)qh_set_composition_type; break;
    case HWC2_FUNCTION_SET_LAYER_BLEND_MODE: *out_fn = (hwc2_function_pointer_t)qh_set_blend_mode; break;
    case HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA: *out_fn = (hwc2_function_pointer_t)qh_set_plane_alpha; break;
    case HWC2_FUNCTION_SET_LAYER_TRANSFORM: *out_fn = (hwc2_function_pointer_t)qh_set_transform; break;
    case HWC2_FUNCTION_SET_LAYER_Z_ORDER: *out_fn = (hwc2_function_pointer_t)qh_set_z; break;
    case HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION: *out_fn = (hwc2_function_pointer_t)qh_set_visible_region; break;
    case HWC2_FUNCTION_SET_DISPLAY_LAYER_STACK: *out_fn = (hwc2_function_pointer_t)qh_set_layer_stack; break;
    case HWC2_FUNCTION_VALIDATE_DISPLAY: *out_fn = (hwc2_function_pointer_t)qh_validate_display; break;
    case HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES: *out_fn = (hwc2_function_pointer_t)qh_accept_changes; break;
    case HWC2_FUNCTION_PRESENT_DISPLAY: *out_fn = (hwc2_function_pointer_t)qh_present; break;
    case HWC2_FUNCTION_GET_RELEASE_FENCES: *out_fn = (hwc2_function_pointer_t)qh_get_release_fences; break;
    case HWC2_FUNCTION_SET_DISPLAY_CLIENT_TARGET: *out_fn = (hwc2_function_pointer_t)qh_set_client_target; break;
    case HWC2_FUNCTION_SET_ACTIVE_CONFIG: *out_fn = (hwc2_function_pointer_t)qh_set_active_config; break;
    case HWC2_FUNCTION_GET_ACTIVE_CONFIG: *out_fn = (hwc2_function_pointer_t)qh_get_active_config; break;
    case HWC2_FUNCTION_SET_DISPLAY_POWER_MODE: *out_fn = (hwc2_function_pointer_t)qh_set_power_mode; break;
    case HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE: *out_fn = (hwc2_function_pointer_t)qh_get_display_attribute; break;
    case HWC2_FUNCTION_GET_DISPLAY_CONFIGS: *out_fn = (hwc2_function_pointer_t)qh_get_display_configs; break;
    default:
        *out_fn = (hwc2_function_pointer_t)0;
        return HWC2_ERROR_UNSUPPORTED;
    }
    return HWC2_ERROR_NONE;
}

/* ------------------------------------------------------------- module glue */
static int qh_close(struct hw_device_t *dev)
{
    struct qh_ctx *c = (struct qh_ctx *)dev;
    if (c->sock >= 0) {
        struct qh_ack bye = {QH_MAGIC, QH_OP_BYE, 0};
        send(c->sock, &bye, sizeof bye, MSG_NOSIGNAL);
        close(c->sock);
    }
    for (int i = 0; i < QH_POOL; i++) {
        if (c->pool[i].addr)
            munmap(c->pool[i].addr, c->pool[i].size);
        if (c->pool[i].fd >= 0)
            close(c->pool[i].fd);
    }
    if (c->sync_timeline_fd >= 0)
        close(c->sync_timeline_fd);
    return 0;
}

static int qh_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    if (id && strcmp(id, HWC_HARDWARE_COMPOSER))
        return -EINVAL;

    struct qh_ctx *c = &g_ctx;
    memset(c, 0, sizeof *c);
    c->sock = -1;
    c->sync_timeline_fd = -1;
    for (int i = 0; i < QH_POOL; i++)
        c->pool[i].fd = -1;

    const char *geom = getenv("QANDROID_DISPLAY");
    unsigned w = QH_DEFAULT_W, h = QH_DEFAULT_H, hz = 60;
    if (geom)
        sscanf(geom, "%ux%u@%u", &w, &h, &hz);
    c->width = w;
    c->height = h;
    c->stride = w;
    c->refresh_hz = hz ? hz : 60;
    c->power_on = 1;
    c->full_redraw = 1;

    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                      (const struct hw_module_t **)&c->gralloc) == 0)
        qh_log("gralloc module ready");
    else
        qh_log("no gralloc module: layer buffers cannot be mapped by the shim path");

    if (qh_pool_init(c)) {
        qh_log("output pool allocation failed: %s", strerror(errno));
        return -ENOMEM;
    }
    qh_open_timeline(c);
    qh_connect(c);

    c->device.common.tag = HARDWARE_DEVICE_TAG;
    c->device.common.version = HWC2_DEVICE_API_VERSION_1_0;
    c->device.common.module = (struct hw_module_t *)module;
    c->device.common.close = qh_close;
    c->device.getCapabilities = qh_get_capabilities;
    c->device.getFunction = qh_get_function;
    *device = &c->device.common;

    qh_log("opened %ux%u@%uHz, socket %s", c->width, c->height, c->refresh_hz, c->sock_path);
    return 0;
}

static struct hw_module_methods_t qh_methods = {.open = qh_open};

/* The module entry point must be a global symbol: hw_get_module() dlsym()s
 * "HMI" out of the shared object. `static` here compiles fine, links fine, and
 * produces a module that can never be loaded - the only hint is a
 * "'HMI' defined but not used" warning. */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HWC2_DEVICE_API_VERSION_1_0,
    .hal_api_version = 0,
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "qandroid software composer",
    .author = "qandroid",
    .methods = &qh_methods,
};
