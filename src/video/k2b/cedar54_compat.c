#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

static int is_k2b_model(void) {
    static const char expected[] = "KICKPI K2B";
    /* Room for one optional NUL and one byte proving the model is too long. */
    char model[sizeof(expected) + 1];
    size_t used = 0;
    int eof = 0;
    int fd = open("/proc/device-tree/model", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    while (used < sizeof(model)) {
        ssize_t count = read(fd, model + used, sizeof(model) - used);
        if (count < 0)
            break;
        if (count == 0) {
            eof = 1;
            break;
        }
        used += (size_t)count;
    }
    /* Never retry close: on Linux its descriptor may already have been freed. */
    if (close(fd) != 0 || !eof)
        return 0;
    return (used == sizeof(expected) - 1 ||
            (used == sizeof(expected) && model[used - 1] == '\0')) &&
           memcmp(model, expected, sizeof(expected) - 1) == 0;
}

static int is_compatible_cedar_fd(int fd) {
    const char *enabled = getenv("CEDAR_K2B_KERNEL54_COMPAT");
    struct utsname info;
    struct stat caller, cedar;

    if (enabled == NULL || strcmp(enabled, "1") != 0)
        return 0;
    if (uname(&info) != 0 || strcmp(info.release, "5.4.125") != 0 ||
        strcmp(info.machine, "aarch64") != 0)
        return 0;
    if (fstat(fd, &caller) != 0 || stat("/dev/cedar_dev", &cedar) != 0 ||
        !S_ISCHR(caller.st_mode) || !S_ISCHR(cedar.st_mode) ||
        caller.st_rdev != cedar.st_rdev)
        return 0;
    return is_k2b_model();
}

/* Fixed arguments preserve the raw third register for a future assembly entry.
 * Configure the environment before starting threads. No identity is cached;
 * this validation path is not intended for signal handlers.
 */
__attribute__((visibility("hidden")))
int k2b_cedar54_ioctl_dispatch(int fd, unsigned long request,
                             unsigned long raw_arg) {
    int saved_errno = errno;
    if (request == 0x804UL && is_compatible_cedar_fd(fd)) {
        errno = saved_errno;
        return 0;
    }
    errno = saved_errno;
    return (int)syscall(SYS_ioctl, fd, request, raw_arg);
}
