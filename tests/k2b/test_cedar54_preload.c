#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int check_provider(const char *expected) {
    Dl_info actual_info, libc_info;
    char *actual_path = NULL;
    char *expected_path = NULL;
    int result = 1;

    if (dladdr((void *)ioctl, &actual_info) == 0) {
        fprintf(stderr, "FAIL: dladdr(ioctl) did not find a provider\n");
        return 1;
    }
    if (strcmp(expected, "--baseline") == 0) {
        if (dladdr((void *)close, &libc_info) == 0 ||
            strcmp(strrchr(libc_info.dli_fname, '/') == NULL ?
                   libc_info.dli_fname : strrchr(libc_info.dli_fname, '/') + 1,
                   "libc.so.6") != 0) {
            fprintf(stderr, "FAIL: cannot identify baseline libc\n");
            return 1;
        }
        expected = libc_info.dli_fname;
    }
    actual_path = realpath(actual_info.dli_fname, NULL);
    expected_path = realpath(expected, NULL);
    if (actual_path == NULL || expected_path == NULL) {
        fprintf(stderr, "FAIL: cannot resolve ioctl provider paths: %s\n",
                strerror(errno));
        goto out;
    }
    if (strcmp(actual_path, expected_path) != 0) {
        fprintf(stderr, "FAIL: ioctl provider %s; expected %s\n",
                actual_path, expected_path);
        goto out;
    }
    printf("PASS: ioctl provider %s\n", actual_path);
    result = 0;
out:
    free(actual_path);
    free(expected_path);
    return result;
}

#define CHECK(condition, description) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (errno=%d)\n", description, errno); \
        goto out; \
    } \
} while (0)

static int check_pipe_ioctls(void) {
    const int sentinel = EDOM;
    const char payload[] = "abc";
    int pipefd[2] = {-1, -1};
    int count = -1;
    int nonblocking = 1;
    int flags;
    int result = 1;

    CHECK(pipe(pipefd) == 0, "create pipe");
    CHECK(write(pipefd[1], payload, sizeof(payload) - 1) ==
          (ssize_t)(sizeof(payload) - 1), "write pipe bytes");
    errno = sentinel;
    CHECK(ioctl(pipefd[0], FIONREAD, &count) == 0, "FIONREAD pointer");
    CHECK(errno == sentinel, "FIONREAD preserves success errno");
    CHECK(count == (int)(sizeof(payload) - 1), "FIONREAD byte count");

    errno = sentinel;
    CHECK(ioctl(pipefd[0], FIONBIO, &nonblocking) == 0, "FIONBIO pointer enable");
    CHECK(errno == sentinel, "FIONBIO preserves success errno");
    flags = fcntl(pipefd[0], F_GETFL);
    CHECK(flags != -1 && (flags & O_NONBLOCK) != 0, "nonblocking enabled");
    nonblocking = 0;
    errno = sentinel;
    CHECK(ioctl(pipefd[0], FIONBIO, &nonblocking) == 0, "FIONBIO pointer disable");
    CHECK(errno == sentinel, "FIONBIO disable preserves success errno");
    flags = fcntl(pipefd[0], F_GETFL);
    CHECK(flags != -1 && (flags & O_NONBLOCK) == 0, "nonblocking disabled");

    errno = sentinel;
    CHECK(ioctl(pipefd[0], FIOCLEX) == 0, "two-argument FIOCLEX");
    CHECK(errno == sentinel, "FIOCLEX preserves success errno");
    CHECK(fcntl(pipefd[0], F_GETFD) == FD_CLOEXEC, "close-on-exec enabled");
    errno = sentinel;
    CHECK(ioctl(pipefd[0], FIONCLEX) == 0, "two-argument FIONCLEX");
    CHECK(errno == sentinel, "FIONCLEX preserves success errno");
    CHECK(fcntl(pipefd[0], F_GETFD) == 0, "close-on-exec disabled");
    errno = sentinel;
    CHECK(ioctl(pipefd[0], FIOCLEX, 0x123456789abcdef0UL) == 0,
          "FIOCLEX with irrelevant raw third argument");
    CHECK(errno == sentinel, "extra argument preserves success errno");
    CHECK(fcntl(pipefd[0], F_GETFD) == FD_CLOEXEC, "extra argument ignored");

    errno = sentinel;
    CHECK(ioctl(-1, FIONREAD, &count) == -1, "invalid fd return");
    CHECK(errno == EBADF, "invalid fd errno");
    errno = sentinel;
    CHECK(ioctl(pipefd[0], 0x7ffffffeUL, 0UL) == -1, "unknown request return");
    CHECK(errno == ENOTTY, "unknown request errno");
    errno = sentinel;
    CHECK(ioctl(pipefd[0], 0x804UL, 0UL) == -1, "0x804 on pipe return");
    CHECK(errno == ENOTTY, "0x804 on pipe is not intercepted");
    result = 0;
out:
    if (pipefd[0] >= 0 && close(pipefd[0]) != 0)
        result = 1;
    if (pipefd[1] >= 0 && close(pipefd[1]) != 0)
        result = 1;
    if (result == 0)
        puts("PASS: pipe ioctl pointer, two-argument, raw-argument and errno ABI");
    return result;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s --baseline|EXPECTED_PROVIDER\n", argv[0]);
        return 2;
    }
    if (check_provider(argv[1]) != 0)
        return 1;
    return check_pipe_ioctls();
}
