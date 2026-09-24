#define _GNU_SOURCE
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>

static atomic_int enabled;
void k2b_network_enable(void) { atomic_store(&enabled, 1); }
static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void resolve(void) { *(void **)(&real_setsockopt) = dlsym(RTLD_NEXT, "setsockopt"); }

/* Export from the executable: moonlight-common is a shared library, so linker's
 * --wrap would only catch calls originating in the executable, not its calls. */
int setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{
    pthread_once(&once, resolve);
    if (!real_setsockopt) { errno = ENOSYS; return -1; }
    int result = real_setsockopt(fd, level, option, value, size);
    int saved_errno = errno;
    if (!result && atomic_load(&enabled) && level == SOL_SOCKET && option == SO_RCVBUF &&
        value && size == sizeof(int)) {
        int requested = *(const int *)value, actual = 0;
        socklen_t len = sizeof(actual);
        if (requested > 0 && requested <= 8 * 1024 * 1024 &&
            !getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len) &&
            actual < requested * 2) {
            /* K2B already runs privileged for disp/VPU. Override the cap on
             * this socket only, not net.core.rmem_max for the whole system. */
            if (real_setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, value, size)) {
                fprintf(stderr, "K2B: receive buffer capped at %d bytes; SO_RCVBUFFORCE unavailable\n", actual);
            } else if (!getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len)) {
                fprintf(stderr, "K2B: socket receive buffer=%d bytes (Linux doubled accounting)\n", actual);
            }
        }
    }
    errno = saved_errno;
    return result;
}
