#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int mode, opened, called, closed;
static int fake_open(const char *path, int flags, ...) {
    assert(!strcmp(path, "/dev/fb0"));
    assert((flags & O_CLOEXEC) && (flags & O_RDWR));
    opened++;
    if (mode == 1) { errno = EACCES; return -1; }
    return 42;
}
static int fake_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    assert(fd == 42 && request == FBIOBLANK);
    assert(va_arg(args, int) == FB_BLANK_UNBLANK);
    va_end(args);
    called++;
    if (mode == 2) { errno = EIO; return -1; }
    return 0;
}
static int fake_close(int fd) { assert(fd == 42); closed++; return 0; }
#define open fake_open
#define ioctl fake_ioctl
#define close fake_close
#define main unblank_main
#include "../../tools/k2b-fb-unblank.c"
#undef main
int main(void) {
    for (mode = 0; mode < 3; mode++) {
        opened = called = closed = 0;
        assert((unblank_main() == 0) == (mode == 0));
        assert(opened == 1);
        assert(called == (mode != 1));
        assert(closed == (mode != 1));
    }
    puts("PASS: fb unblank success/open-failure/ioctl-failure");
    return 0;
}
