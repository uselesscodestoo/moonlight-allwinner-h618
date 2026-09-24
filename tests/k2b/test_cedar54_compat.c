#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

int k2b_cedar54_ioctl_dispatch(int fd, unsigned long request,
                             unsigned long raw_arg);

/* Assertions must still execute when the production build defines NDEBUG. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)

enum operation { ENV, UNAME, FSTAT, STAT, OPEN, READ, CLOSE, SYSCALL, OP_COUNT };
enum fault { NONE, UNAME_FAIL, FSTAT_FAIL, STAT_FAIL, FD_REGULAR, PATH_REGULAR,
             RDEV_MISMATCH, OPEN_FAIL, READ_FAIL, READ_AFTER_DATA, READ_EINTR,
             CLOSE_FAIL, CLOSE_EINTR, READ_AND_CLOSE_FAIL };
static enum fault fault;
static unsigned int calls[OP_COUNT];
static const char *environment = "1", *release = "5.4.125", *machine = "aarch64";
static const char *model = "KICKPI K2B";
static size_t model_size = sizeof("KICKPI K2B"), model_offset, read_chunk;
static int model_fd = 20, model_live, saw_eof;
static int syscall_result = 7, syscall_errno = ENOTTY, entry_errno = EDOM;
static int expected_fd;
static unsigned long expected_request, expected_arg;
static unsigned int cases_run;

static int fail_with(int error) { errno = error; return -1; }

static void identity(enum operation op) {
    ++calls[op];
    /* Successful validation must not leak even incidental errno changes. */
    errno = EBUSY;
}

char *__wrap_getenv(const char *name) {
    identity(ENV);
    CHECK(strcmp(name, "CEDAR_K2B_KERNEL54_COMPAT") == 0);
    return (char *)environment;
}

int __wrap_uname(struct utsname *info) {
    identity(UNAME);
    if (fault == UNAME_FAIL) return fail_with(EIO);
    memset(info, 0, sizeof(*info));
    strcpy(info->release, release);
    strcpy(info->machine, machine);
    return 0;
}

int __wrap_fstat(int fd, struct stat *info) {
    identity(FSTAT);
    CHECK(fd == expected_fd);
    if (fault == FSTAT_FAIL || fd < 0) return fail_with(EBADF);
    memset(info, 0, sizeof(*info));
    info->st_mode = fault == FD_REGULAR ? S_IFREG : S_IFCHR;
    info->st_rdev = 123;
    return 0;
}

int __wrap_stat(const char *path, struct stat *info) {
    identity(STAT);
    CHECK(strcmp(path, "/dev/cedar_dev") == 0);
    if (fault == STAT_FAIL) return fail_with(ENOENT);
    memset(info, 0, sizeof(*info));
    info->st_mode = fault == PATH_REGULAR ? S_IFREG : S_IFCHR;
    info->st_rdev = fault == RDEV_MISMATCH ? 124 : 123;
    return 0;
}

int __wrap_open(const char *path, int flags, ...) {
    identity(OPEN);
    CHECK(strcmp(path, "/proc/device-tree/model") == 0);
    CHECK(flags == (O_RDONLY | O_CLOEXEC));
    CHECK(!model_live);
    if (fault == OPEN_FAIL) return fail_with(EACCES);
    CHECK(model_fd != expected_fd);
    model_live = 1;
    return model_fd;
}

ssize_t __wrap_read(int fd, void *buffer, size_t bytes) {
    size_t count;
    identity(READ);
    CHECK(fd == model_fd && model_live);
    CHECK(bytes > 0 && calls[READ] <= 32);
    if (fault == READ_FAIL || fault == READ_AND_CLOSE_FAIL ||
        (fault == READ_AFTER_DATA && calls[READ] == 2))
        return fail_with(EIO);
    if (fault == READ_EINTR) return fail_with(EINTR);
    if (model_offset == model_size) { saw_eof = 1; return 0; }
    count = model_size - model_offset;
    if (count > bytes) count = bytes;
    if (read_chunk && count > read_chunk) count = read_chunk;
    memcpy(buffer, model + model_offset, count);
    model_offset += count;
    return (ssize_t)count;
}

int __wrap_close(int fd) {
    identity(CLOSE);
    CHECK(fd == model_fd && model_live);
    model_live = 0;
    if (fault == CLOSE_FAIL || fault == READ_AND_CLOSE_FAIL) return fail_with(EIO);
    if (fault == CLOSE_EINTR) return fail_with(EINTR);
    return 0;
}

long __wrap_syscall(long number, ...) {
    va_list args;
    ++calls[SYSCALL];
    CHECK(number == SYS_ioctl);
    va_start(args, number);
    CHECK(va_arg(args, int) == expected_fd);
    CHECK(va_arg(args, unsigned long) == expected_request);
    CHECK(va_arg(args, unsigned long) == expected_arg);
    va_end(args);
    CHECK(errno == entry_errno);
    if (syscall_result == -1) errno = syscall_errno;
    return syscall_result;
}

static void reset(void) {
    CHECK(!model_live);
    fault = NONE;
    environment = "1";
    release = "5.4.125";
    machine = "aarch64";
    model = "KICKPI K2B";
    model_size = sizeof("KICKPI K2B");
    read_chunk = 0;
    model_fd = 20;
    syscall_result = 7;
    syscall_errno = ENOTTY;
    entry_errno = EDOM;
}

static void expect_call(int fd, unsigned long request, unsigned long raw_arg,
                        int result, int forwarded) {
    unsigned int i;
    int actual;
    expected_fd = fd;
    expected_request = request;
    expected_arg = raw_arg;
    memset(calls, 0, sizeof(calls));
    model_offset = 0;
    saw_eof = 0;
    errno = entry_errno;
    actual = k2b_cedar54_ioctl_dispatch(fd, request, raw_arg);
    if (actual != result)
        fprintf(stderr, "case %u: request=%#lx fault=%d model_size=%zu actual=%d expected=%d\n",
                cases_run + 1, request, fault, model_size, actual, result);
    CHECK(actual == result);
    CHECK(errno == (forwarded && syscall_result == -1 ? syscall_errno : entry_errno));
    CHECK(calls[SYSCALL] == (unsigned int)forwarded);
    CHECK(!model_live);
    CHECK(calls[CLOSE] == (calls[OPEN] && fault != OPEN_FAIL ? 1U : 0U));
    for (i = 0; i < OP_COUNT; ++i)
        CHECK(calls[i] <= (i == READ ? 32U : 1U));
    if (request != 0x804UL)
        for (i = 0; i < SYSCALL; ++i) CHECK(calls[i] == 0);
    if (!forwarded) {
        CHECK(calls[ENV] == 1 && calls[UNAME] == 1);
        CHECK(calls[FSTAT] == 1 && calls[STAT] == 1);
        CHECK(calls[OPEN] == 1 && calls[CLOSE] == 1 && saw_eof);
    }
    ++cases_run;
}

static void test_identity(void) {
    static const char *const disabled[] = { NULL, "", "0", "01", "10", "true", "yes", " 1", "1 ", "1\n" };
    static const char *const releases[] = { "", "5.4.12", "5.4.125-extra", "5.4.125\n", "5.4.126", "6.1.0" };
    static const char *const machines[] = { "", "armv7l", "x86_64", "aarch64-extra", "AARCH64" };
    size_t i;
    for (i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        reset(); environment = disabled[i];
        expect_call(11, 0x804UL, ULONG_MAX, 7, 1);
        CHECK(calls[ENV] == 1 && calls[UNAME] == 0 && calls[OPEN] == 0);
    }
    for (i = 0; i < sizeof(releases) / sizeof(releases[0]); ++i) {
        reset(); release = releases[i];
        expect_call(11, 0x804UL, 0, 7, 1);
    }
    for (i = 0; i < sizeof(machines) / sizeof(machines[0]); ++i) {
        reset(); machine = machines[i];
        expect_call(11, 0x804UL, 0, 7, 1);
    }
    for (i = UNAME_FAIL; i <= READ_AND_CLOSE_FAIL; ++i) {
        reset(); fault = (enum fault)i;
        if (fault == READ_AFTER_DATA) read_chunk = 3;
        expect_call(11, 0x804UL, 0, 7, 1);
    }
    reset(); expect_call(-1, 0x804UL, ULONG_MAX, 7, 1);
    CHECK(calls[OPEN] == 0);
    reset(); expect_call(0, 0x804UL, 1, 0, 0);
}

static void test_model(void) {
    static const struct { const char *data; size_t size; } invalid[] = {
        { "KICKPI K2B X", sizeof("KICKPI K2B X") - 1 },
        { "KICKPI K2B\n", sizeof("KICKPI K2B\n") - 1 },
        { "KICKPI K2B\0\0", sizeof("KICKPI K2B\0\0") - 1 },
        { "KICKPI K2B\0X", sizeof("KICKPI K2B\0X") - 1 },
        { "KICKPI\0K2B", sizeof("KICKPI\0K2B") - 1 },
        { "KICKPI K2C", sizeof("KICKPI K2C") - 1 },
        { " KICKPI K2B", sizeof(" KICKPI K2B") - 1 }
    };
    char long_model[4096];
    size_t i, chunk;
    int fd;
    for (i = sizeof("KICKPI K2B") - 1; i <= sizeof("KICKPI K2B"); ++i)
        for (chunk = 1; chunk <= 11; ++chunk)
            for (fd = 0; fd <= 20; fd += 20) {
                reset(); model_size = i; read_chunk = chunk; model_fd = fd;
                expect_call(11, 0x804UL, ULONG_MAX, 0, 0);
                CHECK(model_offset == model_size);
            }
    for (i = 0; i < sizeof("KICKPI K2B") - 1; ++i) {
        reset(); model_size = i;
        expect_call(11, 0x804UL, 0, 7, 1);
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reset(); model = invalid[i].data; model_size = invalid[i].size; read_chunk = 1;
        expect_call(11, 0x804UL, 0, 7, 1);
    }
    memset(long_model, 'X', sizeof(long_model));
    memcpy(long_model, "KICKPI K2B", sizeof("KICKPI K2B"));
    reset(); model = long_model; model_size = sizeof(long_model);
    expect_call(11, 0x804UL, 0, 7, 1);
    CHECK(model_offset <= sizeof("KICKPI K2B") + 1); /* Stop once too long is proven. */
    reset(); fault = READ_AFTER_DATA; read_chunk = 10;
    expect_call(11, 0x804UL, 0, 7, 1); /* Valid prefix without EOF is insufficient. */
}

static void test_forwarding(void) {
    static const unsigned long requests[] = {
        0, 0x803UL, 0x805UL, _IO('x', 1), _IOR('x', 2, int),
        _IOW('x', 3, int), ULONG_MAX,
#if ULONG_MAX > 0xffffffffUL
        0x100000804UL, 0xffffffff00000804UL,
#endif
    };
    unsigned long args[] = { 0, 1, ULONG_MAX, 0xdeadbeefUL, (unsigned long)(uintptr_t)&cases_run };
    static const int results[] = { 7, 0, -1 };
    size_t i, j, k;
    for (i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i)
        for (j = 0; j < sizeof(args) / sizeof(args[0]); ++j)
            for (k = 0; k < sizeof(results) / sizeof(results[0]); ++k) {
                reset(); syscall_result = results[k]; syscall_errno = EINTR;
                expect_call(-17, requests[i], args[j], results[k], 1);
            }
    for (i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        reset(); syscall_result = results[i]; fault = READ_FAIL;
        expect_call(11, 0x804UL, args[4], results[i], 1);
    }
    reset(); entry_errno = 0;
    expect_call(11, 0x804UL, args[4], 0, 0);
    environment = "0";
    expect_call(11, 0x804UL, args[4], 7, 1);
    environment = "1"; fault = RDEV_MISMATCH;
    expect_call(11, 0x804UL, args[4], 7, 1);
    fault = NONE;
    expect_call(11, 0x804UL, args[4], 0, 0);
    model = "OTHER  K2B";
    expect_call(11, 0x804UL, args[4], 7, 1);
}

int main(void) {
    alarm(10);
    reset();
    environment = NULL;
    expect_call(11, 0x804UL, 0, 7, 1);
    reset();
    expect_call(11, 0x804UL, 0, 0, 0);
    expect_call(11, 0x123UL, 0xabcdefUL, 7, 1);
    test_identity();
    test_model();
    test_forwarding();
    printf("cedar54 compat: %u cases passed\n", cases_run);
    return 0;
}
