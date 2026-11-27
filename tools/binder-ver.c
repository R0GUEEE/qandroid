/*
 * binder-ver - prove whether a binder device works, from the *host* side.
 *
 *   binder-ver /path/to/binder
 *
 * Opens the node and asks for its protocol version (BINDER_VERSION). If this
 * succeeds on the host but the same call fails with ENOTTY from inside
 * qemu-aarch64, the problem is in the translation layer, not the driver or the
 * device node - which is exactly the kind of split that otherwise takes several
 * six-minute CI runs to establish.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BINDER_VERSION _IOWR('b', 9, int32_t)

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <binder-node>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    int32_t version = -1;
    if (ioctl(fd, BINDER_VERSION, &version) < 0) {
        fprintf(stderr, "BINDER_VERSION (0x%lx) on %s: %s\n",
                (unsigned long)BINDER_VERSION, argv[1], strerror(errno));
        close(fd);
        return 1;
    }
    printf("%s: binder protocol %d\n", argv[1], version);
    close(fd);
    return 0;
}
