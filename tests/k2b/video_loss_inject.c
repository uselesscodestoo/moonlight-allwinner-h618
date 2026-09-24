/* Test-only executable interposer: discard video UDP packets in this process.
 * Never changes interface, firewall, SSH, audio or input traffic. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static ssize_t (*receive_real)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void resolve(void) { *(void **)(&receive_real) = dlsym(RTLD_NEXT, "recvfrom"); }
ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *address, socklen_t *length)
{
    pthread_once(&once, resolve);
    if (!receive_real) { errno = ENOSYS; return -1; }
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    ssize_t result = receive_real(fd, data, size, flags,
        address ? address : (struct sockaddr *)&peer, address ? length : &peer_length);
    const char *mode = getenv("K2B_TEST_VIDEO_LOSS");
    if (result <= 0 || !mode) return result;
    struct sockaddr *source = address ? address : (struct sockaddr *)&peer;
    int port = source->sa_family == AF_INET ? ntohs(((struct sockaddr_in *)source)->sin_port) :
               source->sa_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)source)->sin6_port) : 0;
    if (port != 47998) return result;
    static long long first_ms;
    static unsigned packets;
    static int previous;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!first_ms) first_ms = now;
    long long elapsed = now - first_ms;
    int injecting = elapsed >= 10000 && (strcmp(mode, "short") || elapsed < 14000);
    if (injecting != previous) {
        fprintf(stderr, "TEST: video loss %s at %lld ms mode=%s\n", injecting ? "ON" : "OFF", elapsed, mode);
        previous = injecting;
    }
    if (injecting && (strcmp(mode, "partial") || ++packets % 4)) {
        errno = EAGAIN;
        return -1;
    }
    return result;
}
