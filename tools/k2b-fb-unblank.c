#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }
    int result = ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK);
    if (result < 0) perror("FBIOBLANK UNBLANK");
    close(fd);
    if (result < 0) return 1;
    puts("K2B desktop: fb0 layer enabled");
    return 0;
}
