#define _GNU_SOURCE
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#ifdef TEST_K2B_NETWORK
void k2b_network_enable(void);
#endif
int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int want = 3 * 1024 * 1024, actual = 0;
    socklen_t size = sizeof(actual);
#ifdef TEST_K2B_NETWORK
    k2b_network_enable();
#endif
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want)) ||
        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &size)) return 2;
    close(fd);
    printf("UDP requested=%d actual=%d (Linux reports doubled accounting)\n", want, actual);
    return actual >= 2 * want ? 0 : 1;
}
