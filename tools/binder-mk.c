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
 *   BINDERFS_CTL_ADD = _IOW('b', 1, struct binderfs_device)
 *   struct binderfs_device { char name[256]; __u32 major, minor; }
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

#ifndef BINDERFS_MAX_NAME
#define BINDERFS_MAX_NAME 255
#endif

struct binderfs_device {
    char name[BINDERFS_MAX_NAME + 1];
    unsigned int major;
    unsigned int minor;
};

#define BINDERFS_CTL_ADD _IOW('b', 1, struct binderfs_device)

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <binderfs-mountpoint> <name>\n", argv[0]);
        return 2;
    }
    const char *mp = argv[1], *name = argv[2];
    if (strlen(name) > BINDERFS_MAX_NAME) {
        fprintf(stderr, "%s: name too long\n", name);
        return 2;
    }

    char ctl[4096];
    snprintf(ctl, sizeof ctl, "%s/binder-control", mp);
    int fd = open(ctl, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", ctl, strerror(errno));
        return 1;
    }

    struct binderfs_device dev;
    memset(&dev, 0, sizeof dev);
    strncpy(dev.name, name, BINDERFS_MAX_NAME);
    if (ioctl(fd, BINDERFS_CTL_ADD, &dev) < 0) {
        /* EEXIST means the node is already provisioned - not an error here */
        if (errno != EEXIST) {
            fprintf(stderr, "BINDERFS_CTL_ADD %s: %s\n", name, strerror(errno));
            close(fd);
            return 1;
        }
        close(fd);
        return 0;
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
