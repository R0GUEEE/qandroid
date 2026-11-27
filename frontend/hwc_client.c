/*
 * hwc_client - host side of the qandroid display protocol.
 *
 * Listens on a unix socket, receives frame headers + memfd-backed buffers from
 * the guest composer, blits them into an SDL window, and turns window input
 * into a virtual touchscreen device (uinput) that the guest's InputReader picks
 * up as if it were real hardware.
 *
 *   ./hwc_client --socket ~/.local/share/qandroid/hwc.sock --fullscreen
 *
 * Build: cc -O2 -o hwc_client hwc_client.c $(pkg-config --cflags --libs sdl2)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "qandroid_hwc_proto.h"

struct app {
    const char *sock_path;
    int listen_fd;
    int conn;
    int uinput;
    int uinput_w, uinput_h;
    float scale;
    int fullscreen;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    uint32_t tex_w, tex_h;
    uint32_t frame_w, frame_h;
    int running;
};

static void die(const char *msg)
{
    fprintf(stderr, "hwc_client: %s: %s\n", msg, strerror(errno));
    exit(1);
}

/* ---------------------------------------------------------------- uinput */
static int uinput_open(struct app *a, uint32_t w, uint32_t h)
{
    static const char *paths[] = {"/dev/uinput", "/dev/input/uinput", NULL};
    int fd = -1;
    for (int i = 0; paths[i]; i++) {
        fd = open(paths[i], O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
            break;
    }
    if (fd < 0) {
        fprintf(stderr, "hwc_client: no uinput (%s) - display works, input does not\n",
                strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "qandroid virtual touchscreen");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x5141;
    dev.id.product = 0x0001;
    dev.id.version = 1;
    dev.absmin[ABS_X] = 0;
    dev.absmax[ABS_X] = (int)w - 1;
    dev.absmin[ABS_Y] = 0;
    dev.absmax[ABS_Y] = (int)h - 1;
    if (write(fd, &dev, sizeof dev) != (ssize_t)sizeof dev) {
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    a->uinput_w = (int)w;
    a->uinput_h = (int)h;
    printf("hwc_client: virtual touchscreen %ux%u created (InputReader picks it up on rescan)\n",
           w, h);
    return fd;
}

static void uinput_emit(struct app *a, int type, int code, int value)
{
    if (a->uinput < 0)
        return;
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (uint16_t)type;
    ev.code = (uint16_t)code;
    ev.value = value;
    if (write(a->uinput, &ev, sizeof ev) != (ssize_t)sizeof ev)
        return;
}

static void uinput_touch(struct app *a, int down, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= a->uinput_w) x = a->uinput_w - 1;
    if (y >= a->uinput_h) y = a->uinput_h - 1;
    uinput_emit(a, EV_ABS, ABS_X, x);
    uinput_emit(a, EV_ABS, ABS_Y, y);
    uinput_emit(a, EV_KEY, BTN_TOUCH, down);
    uinput_emit(a, EV_SYN, SYN_REPORT, 0);
}

/* ---------------------------------------------------------------- socket */
static int listen_on(const char *path)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    unlink(path); /* stale from a previous run */
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        die("socket");
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("bind");
    chmod(path, 0666);
    if (listen(fd, 1) < 0)
        die("listen");
    return fd;
}

/* --------------------------------------------------------------- frames */
static int present_frame(struct app *a, const struct qh_frame *f, int fd)
{
    size_t need = (size_t)f->stride * f->height * 4;
    void *p = mmap(NULL, need, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "hwc_client: mmap frame: %s\n", strerror(errno));
        return -1;
    }
    if (!a->tex || a->tex_w != f->width || a->tex_h != f->height) {
        if (a->tex)
            SDL_DestroyTexture(a->tex);
        a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_STREAMING, (int)f->width, (int)f->height);
        a->tex_w = f->width;
        a->tex_h = f->height;
        SDL_RenderSetLogicalSize(a->ren, (int)f->width, (int)f->height);
        if (a->uinput < 0 || a->uinput_w != (int)f->width || a->uinput_h != (int)f->height) {
            if (a->uinput >= 0)
                close(a->uinput);
            a->uinput = uinput_open(a, f->width, f->height);
        }
        a->frame_w = f->width;
        a->frame_h = f->height;
    }
    SDL_UpdateTexture(a->tex, NULL, p, (int)f->stride * 4);
    munmap(p, need);

    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, NULL);
    SDL_RenderPresent(a->ren);
    return 0;
}

static int handle_message(struct app *a)
{
    struct qh_frame f;
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof ctrl);
    struct iovec iov = {.iov_base = &f, .iov_len = sizeof f};
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;

    ssize_t n = recvmsg(a->conn, &mh, MSG_CMSG_CLOEXEC);
    if (n <= 0)
        return -1;

    int got_fd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
            memcpy(&got_fd, CMSG_DATA(cm), sizeof(int));
    }

    if (f.magic != QH_MAGIC) {
        fprintf(stderr, "hwc_client: bad magic 0x%08x\n", f.magic);
        if (got_fd >= 0)
            close(got_fd);
        return 0;
    }
    if (f.op == QH_OP_BYE) {
        printf("hwc_client: guest shut the composer down\n");
        return -1;
    }
    if (f.op != QH_OP_FRAME) {
        if (got_fd >= 0)
            close(got_fd);
        return 0;
    }
    if (got_fd >= 0) {
        present_frame(a, &f, got_fd);
        close(got_fd);
    }
    struct qh_ack ack = {QH_MAGIC, QH_OP_ACK, f.seq};
    send(a->conn, &ack, sizeof ack, MSG_NOSIGNAL);
    return 0;
}

/* ------------------------------------------------------------------ main */
static void parse_args(struct app *a, int argc, char **argv)
{
    a->scale = 1.0f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            a->sock_path = argv[++i];
        else if (!strcmp(argv[i], "--fullscreen"))
            a->fullscreen = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            a->scale = (float)atof(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--socket PATH] [--fullscreen] [--scale F]\n", argv[0]);
            exit(2);
        }
    }
    if (!a->sock_path) {
        const char *env = getenv(QH_SOCK_ENV);
        a->sock_path = env ? env : "/tmp/qandroid-hwc.sock";
    }
}

int main(int argc, char **argv)
{
    struct app a;
    memset(&a, 0, sizeof a);
    a.conn = -1;
    a.uinput = -1;
    parse_args(&a, argc, argv);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "hwc_client: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    a.win = SDL_CreateWindow("qandroid", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)(1280 * a.scale), (int)(720 * a.scale),
                             SDL_WINDOW_RESIZABLE | (a.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    a.ren = SDL_CreateRenderer(a.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!a.ren)
        a.ren = SDL_CreateRenderer(a.win, -1, SDL_RENDERER_SOFTWARE);
    SDL_SetWindowTitle(a.win, "qandroid - waiting for guest");

    a.listen_fd = listen_on(a.sock_path);
    printf("hwc_client: listening on %s\n", a.sock_path);
    a.running = 1;

    while (a.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                a.running = 0;
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEMOTION: {
                if (!a.frame_w)
                    break;
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                SDL_Point pt = {mx, my};
                float lx, ly;
                SDL_RenderWindowToLogical(a.ren, pt.x, pt.y, &lx, &ly);
                int down = (ev.type == SDL_MOUSEMOTION)
                               ? (ev.motion.state & SDL_BUTTON_LMASK) != 0
                               : ev.button.state == SDL_PRESSED;
                uinput_touch(&a, down, (int)lx, (int)ly);
                break;
            }
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE)
                    a.running = 0;
                break;
            }
        }

        struct pollfd pfds[2];
        int n = 0;
        pfds[n].fd = a.listen_fd;
        pfds[n++].events = POLLIN;
        if (a.conn >= 0) {
            pfds[n].fd = a.conn;
            pfds[n++].events = POLLIN;
        }
        int r = poll(pfds, (nfds_t)n, 10);
        if (r <= 0)
            continue;
        if (a.conn < 0 && (pfds[0].revents & POLLIN)) {
            a.conn = accept(a.listen_fd, NULL, NULL);
            if (a.conn >= 0) {
                printf("hwc_client: guest composer connected\n");
                SDL_SetWindowTitle(a.win, "qandroid");
                SDL_RenderSetLogicalSize(a.ren, 1280, 720);
                if (a.uinput < 0)
                    a.uinput = uinput_open(&a, 1280, 720);
            }
            continue;
        }
        if (a.conn >= 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            if (handle_message(&a) < 0) {
                close(a.conn);
                a.conn = -1;
                SDL_SetWindowTitle(a.win, "qandroid - guest disconnected");
            }
        }
    }

    if (a.conn >= 0)
        close(a.conn);
    close(a.listen_fd);
    unlink(a.sock_path);
    if (a.uinput >= 0) {
        ioctl(a.uinput, UI_DEV_DESTROY);
        close(a.uinput);
    }
    SDL_Quit();
    return 0;
}
