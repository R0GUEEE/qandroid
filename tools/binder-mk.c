/*
 * binder-mk - create a binderfs device node without needing the `binderfs`
 * mount(8) helper.  Used by qandroid to expose /dev/binder, /dev/hwbinder and
 * /dev/vndbinder to the arm64 guest.
 *
 *   binder-mk /dev/binderfs binder
 *
 * binderfs itself is architecture independent: it is a host kernel filesystem,
 * so the 32/64-bit guests share it.  Node creation is an ioctl on
 * <mount>/binder-control:
 *
 *   BINDER_CTL_ADD = _IOWR('b', 1, struct binderfs_device)
 *
 * The direction bits matter: with _IOW the kernel sees a different command
 * number and the call comes back EINVAL, which reads like a bad struct but is
 * really a bad constant.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Fallback definition, checked against the kernel header below when present. */
#ifndef BINDERFS_MAX_NAME
#define BINDERFS_MAX_NAME 255
#endif
struct qh_binderfs_device {
    char name[BINDERFS_MAX_NAME + 1];
    unsigned int major;
    unsigned int minor;
};
#define QH_FALLBACK_CTL_ADD _IOWR('b', 1, struct qh_binderfs_device)

#if defined(__has_include) && __has_include(<linux/android/binderfs.h>)
#include <linux/android/binderfs.h>
#define QH_HAVE_KERNEL_HDR 1
_Static_assert(BINDER_CTL_ADD == QH_FALLBACK_CTL_ADD,
               "binderfs ioctl constant disagrees with <linux/android/binderfs.h>");
_Static_assert(sizeof(struct binderfs_device) == sizeof(struct qh_binderfs_device),
               "binderfs_device layout disagrees with the kernel header");
#define QH_CTL_ADD BINDER_CTL_ADD
typedef struct binderfs_device qh_device_t;
#else
#define QH_HAVE_KERNEL_HDR 0
#define QH_CTL_ADD QH_FALLBACK_CTL_ADD
typedef struct qh_binderfs_device qh_device_t;
#endif

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <binderfs-mountpoint> <name>\n", argv[0]);
        return 2;
    }
    const char *mp = argv[1], *name = argv[2];
    if (!*name || strlen(name) > BINDERFS_MAX_NAME) {
        fprintf(stderr, "%s: invalid device name\n", name);
        return 2;
    }

    char ctl[4096];
    snprintf(ctl, sizeof ctl, "%s/binder-control", mp);
    int fd = open(ctl, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s (is binderfs mounted there?)\n", ctl, strerror(errno));
        return 1;
    }

    qh_device_t dev;
    memset(&dev, 0, sizeof dev);
    strncpy(dev.name, name, BINDERFS_MAX_NAME);
    if (ioctl(fd, QH_CTL_ADD, &dev) < 0) {
        int e = errno;
        close(fd);
        if (e == EEXIST) { /* already provisioned: fine */
            printf("%s/%s (exists)\n", mp, name);
            return 0;
        }
        fprintf(stderr, "BINDER_CTL_ADD %s: %s\n", name, strerror(e));
        return 1;
    }
    close(fd);

    char path[4096];
    snprintf(path, sizeof path, "%s/%s", mp, name);
    if (mknod(path, S_IFCHR | 0666, makedev(dev.major, dev.minor)) < 0 && errno != EEXIST) {
        fprintf(stderr, "mknod %s: %s\n", path, strerror(errno));
        return 1;
    }
    chmod(path, 0666);
    printf("%s %u:%u\n", path, dev.major, dev.minor);
    return 0;
}
