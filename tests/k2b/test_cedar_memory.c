#define _GNU_SOURCE
#include "cedar_memory.h"
#include "cedar_uapi.h"
#include <sc_interface.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* Only syscalls are replaced. CHECK deliberately remains active under NDEBUG. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
enum fault { NONE, UNAME_FAIL, RELEASE_BAD, MACHINE_BAD, PAGE_BAD, MODEL_OPEN,
    MODEL_READ, MODEL_CLOSE, MODEL_BAD, MODEL_SHORT, MODEL_EXTRA, HEAP_OPEN,
    HEAP_STAT, HEAP_TYPE, VE_OPEN, VE_STAT, VE_TYPE, REQUEST_FAIL, RELEASE_FAIL,
    ALLOC_FAIL, MMAP_FAIL, MAP_FAIL, ADDRESS_ZERO, ADDRESS_OVERFLOW,
    ADDRESS_OVERLAP, UNMAP_FAIL, MUNMAP_FAIL, DMA_CLOSE, VE_CLOSE, HEAP_CLOSE,
    FLUSH_FAIL };
static enum fault fault;
static int model_open, heap_open, ve_open, next_dma = 20;
static int requests, releases, maps, unmaps, flushes, alloc_calls, reads, io_calls;
static size_t last_alloc, event_count;
static struct cache_range last_flush;
static char events[2048];
struct fake_dma { int fd, open, mapped; void *ptr; size_t bytes; uint32_t address; };
static struct fake_dma dma[16];
static size_t dma_count;
static unsigned int cases_run;
static void event(char c) { CHECK(event_count + 1 < sizeof(events)); events[event_count++] = c; events[event_count] = 0; ++io_calls; }
static int failure(void) { errno = EIO; return -1; }
static struct fake_dma *by_fd(int fd) { size_t i; for (i = 0; i < dma_count; ++i) if (dma[i].fd == fd) return &dma[i]; CHECK(0); return NULL; }
int __wrap_open(const char *p, int flags, ...) {
    if (!strcmp(p, "/proc/device-tree/model")) {
        event('o'); CHECK(flags == (O_RDONLY | O_CLOEXEC)); if (fault == MODEL_OPEN) return failure();
        CHECK(!model_open); model_open = 1; reads = 0; return 10;
    }
    CHECK(flags == (O_RDWR | O_CLOEXEC));
    if (!strcmp(p, "/dev/cedar_test_heap")) {
        event('h'); if (fault == HEAP_OPEN) return failure(); CHECK(!heap_open); heap_open = 1; return 11;
    }
    CHECK(!strcmp(p, "/dev/cedar_dev")); event('v');
    if (fault == VE_OPEN) return failure();
    CHECK(!ve_open); ve_open = 1; return 12;
}
ssize_t __wrap_read(int fd, void *p, size_t n) {
    const char *s = "KICKPI K2B"; size_t bytes = sizeof("KICKPI K2B"), offset = (size_t)reads * 3;
    CHECK(fd == 10 && model_open); event('r');
    if (fault == MODEL_READ) return failure();
    if (fault == MODEL_BAD) s = "OTHER  K2B";
    if (fault == MODEL_SHORT) bytes = 8;
    if (fault == MODEL_EXTRA) { s = "KICKPI K2B\0EXTRA"; bytes = 15; }
    ++reads; if (offset >= bytes) return 0;
    bytes -= offset; if (bytes > 3) bytes = 3; if (bytes > n) bytes = n;
    memcpy(p, s + offset, bytes); return (ssize_t)bytes;
}
int __wrap_close(int fd) {
    if (fd == 10) { event('c'); CHECK(model_open); model_open = 0; return fault == MODEL_CLOSE ? failure() : 0; }
    if (fd == 11) { event('H'); CHECK(heap_open); heap_open = 0; return fault == HEAP_CLOSE ? failure() : 0; }
    if (fd == 12) { event('V'); CHECK(ve_open); ve_open = 0; return fault == VE_CLOSE ? failure() : 0; }
    { struct fake_dma *d = by_fd(fd); event('D'); CHECK(d->open && !d->mapped); d->open = 0; return fault == DMA_CLOSE ? failure() : 0; }
}
int __wrap_fstat(int fd, struct stat *s) {
    event('s'); CHECK(fd == 11 || fd == 12);
    if ((fd == 11 && fault == HEAP_STAT) || (fd == 12 && fault == VE_STAT)) return failure();
    memset(s, 0, sizeof(*s)); s->st_mode = ((fd == 11 && fault == HEAP_TYPE) || (fd == 12 && fault == VE_TYPE)) ? S_IFREG : S_IFCHR; return 0;
}
int __wrap_uname(struct utsname *u) {
    event('i'); if (fault == UNAME_FAIL) return failure(); memset(u, 0, sizeof(*u));
    strcpy(u->release, fault == RELEASE_BAD ? "5.4.125-extra" : "5.4.125");
    strcpy(u->machine, fault == MACHINE_BAD ? "armv7l" : "aarch64"); return 0;
}
long __wrap_sysconf(int name) { event('p'); CHECK(name == _SC_PAGESIZE); return fault == PAGE_BAD ? 16384 : 4096; }
int __wrap_ioctl(int fd, unsigned long op, ...) {
    va_list a; va_start(a, op);
    if (op == K2B_CEDAR_HEAP_ALLOC) {
        struct fake_dma *d; event('A'); ++alloc_calls; CHECK(fd == 11 && heap_open);
        last_alloc = va_arg(a, unsigned long); va_end(a);
        CHECK(last_alloc && last_alloc <= 32U*1024*1024 && last_alloc % 4096 == 0);
        if (fault == ALLOC_FAIL) return failure();
        CHECK(dma_count < 16); d = &dma[dma_count++]; d->fd = next_dma++; d->open = 1; d->bytes = last_alloc;
        return d->fd;
    }
    CHECK(fd == 12 && ve_open);
    if (op == IOCTL_ENGINE_REQ || op == IOCTL_ENGINE_REL) {
        CHECK(va_arg(a, unsigned long) == 0); va_end(a);
        if (op == IOCTL_ENGINE_REQ) { event('Q'); ++requests; return fault == REQUEST_FAIL ? failure() : 0; }
        event('R'); ++releases; return fault == RELEASE_FAIL ? failure() : 0;
    }
    if (op == IOCTL_FLUSH_CACHE_RANGE) {
        struct cache_range *r = va_arg(a, struct cache_range *); last_flush = *r;
        event('F'); ++flushes; va_end(a); return fault == FLUSH_FAIL ? failure() : 0;
    }
    { struct dma_buf_param *p = va_arg(a, struct dma_buf_param *); struct fake_dma *d = by_fd(p->fd); va_end(a);
        CHECK(d->open && d->ptr);
        if (op == IOCTL_MAP_DMA_BUF) {
            event('M'); ++maps; CHECK(!d->mapped); d->mapped = 1;
            /* Failure can happen after kernel registration, before copy_to_user. */
            if (fault == MAP_FAIL) return failure();
            d->address = 0x10000000U + (uint32_t)(dma_count - 1) * 0x04000000U;
            if (fault == ADDRESS_ZERO) d->address = 0;
            if (fault == ADDRESS_OVERFLOW) d->address = UINT32_MAX - 1024;
            if (fault == ADDRESS_OVERLAP) d->address = dma[0].address + 1024;
            p->phy_addr = d->address; return 0;
        }
        CHECK(op == IOCTL_UNMAP_DMA_BUF); event('U'); ++unmaps; CHECK(d->mapped && p->phy_addr == d->address);
        if (fault == UNMAP_FAIL) return failure();
        d->mapped = 0; return 0;
    }
}
void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off) {
    struct fake_dma *d = by_fd(fd); event('m');
    CHECK(!p && n == d->bytes && prot == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED && off == 0 && d->open);
    if (fault == MMAP_FAIL) { errno = ENOMEM; return MAP_FAILED; }
    d->ptr = malloc(n); CHECK(d->ptr); return d->ptr;
}
int __wrap_munmap(void *p, size_t n) {
    size_t i; event('u');
    for (i = 0; i < dma_count; ++i) if (dma[i].ptr == p) {
        CHECK(n == dma[i].bytes && !dma[i].mapped); if (fault == MUNMAP_FAIL) return failure();
        free(p); dma[i].ptr = NULL; return 0;
    }
    CHECK(0); return -1;
}
static struct k2b_cedar_memory_status status(void) { struct k2b_cedar_memory_status s; CHECK(k2b_cedar_memory_status(&s) == 0); return s; }
static struct ScMemOpsS *start(void) { CHECK(k2b_cedar_memory_begin() == 0); CHECK(MemAdapterGetOpsS()->open() == 0); return MemAdapterGetOpsS(); }
static void no_resources(void) {
    size_t i; struct k2b_cedar_memory_status s = status();
    CHECK(!s.active && !s.allocations && !s.references && !s.live_bytes && !s.pinned && !s.quarantined);
    CHECK(!model_open && !heap_open && !ve_open);
    for (i = 0; i < dma_count; ++i) CHECK(!dma[i].open && !dma[i].mapped && !dma[i].ptr);
}
static void finish(struct ScMemOpsS *ops, int failed) { ops->close(); CHECK(k2b_cedar_memory_end() == (failed ? -1 : 0)); no_resources(); }
static void test_normal(void) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(5000, NULL, NULL); struct k2b_cedar_buffer b; char debug[8];
    CHECK(p && last_alloc == 8192); CHECK(status().live_bytes == 8192 && status().allocations == 1);
    CHECK(k2b_cedar_memory_describe((char *)p + 100, &b) == 0);
    CHECK(b.base == p && b.offset == 100 && b.bytes == 8192 && b.fd == 20 && b.ve_address == 0x10000000);
    CHECK(ops->ve_get_phyaddr((char *)p + 8191) == (void *)(uintptr_t)0x10001fff);
    CHECK(ops->cpu_get_phyaddr(p) == (void *)(uintptr_t)0x10000000);
    CHECK(ops->ve_get_viraddr((void *)(uintptr_t)0x10000064) == (char *)p + 100);
    CHECK(ops->cpu_get_viraddr((void *)(uintptr_t)0x10000000) == p);
    CHECK(!ops->ve_get_phyaddr((char *)p + 8192) && !ops->ve_get_viraddr((void *)1));
    CHECK(ops->get_fd_by_vir((char *)p + 4) == 20 && ops->get_fd_by_vir(NULL) == -1 && !status().error);
    ops->flush_cache(NULL, 0); CHECK(flushes == 0);
    ops->flush_cache((char *)p + 100, 8092); CHECK(flushes == 1 && last_flush.start == (uintptr_t)p + 100 && last_flush.end == (uintptr_t)p + 8192);
    CHECK(ops->mem_set(p, 0x5a, 32) == 0 && ((char *)p)[31] == 0x5a);
    CHECK(ops->mem_cpy((char *)p + 32, p, 32) == 0 && ((char *)p)[63] == 0x5a);
    CHECK(ops->mem_read((char *)p + 64, p, 32) == 0 && ops->mem_write((char *)p + 96, p, 32) == 0);
    CHECK(ops->setup() == 0 && ops->shutdown() == 0 && ops->total_size() == 96*1024 && ops->get_ve_addr_offset() == 0);
    CHECK(SecureMemAdapterGetOpsS() == NULL && MemAdapterGetDramFreq() == -1);
    memset(debug, 'x', sizeof(debug)); CHECK(ops->get_debug_info(debug, sizeof(debug)) == 7 && debug[7] == 0);
    debug[0] = 'x'; CHECK(ops->get_debug_info(debug, 1) == 0 && !debug[0]);
    CHECK(ops->get_debug_info(NULL, 8) == -1 && ops->get_debug_info(debug, 0) == -1 && !status().error);
    CHECK(k2b_cedar_memory_begin() == -1 && errno == EBUSY && !status().error);
    CHECK(ops->open2(NULL, NULL) == 0 && status().references == 2); ops->close();
    CHECK(k2b_cedar_memory_end() == -1 && errno == EBUSY && !releases);
    ops->pfree(NULL, NULL, NULL); ops->pfree(p, NULL, NULL); CHECK(strstr(events, "UuD") != NULL);
    CHECK(status().peak_bytes == 8192); finish(ops, 0); CHECK(strstr(events, "RVH") != NULL);
    CHECK(k2b_cedar_memory_begin() == 0); CHECK(status().references == 0); CHECK(k2b_cedar_memory_end() == 0); no_resources();
}
static void test_fd_zero(void) { struct ScMemOpsS *ops; void *p; next_dma = 0; ops = start(); p = ops->palloc(1, NULL, NULL); CHECK(p && last_alloc == 4096 && ops->get_fd_by_vir(p) == 0); ops->pfree(p, NULL, NULL); finish(ops, 0); }
static void test_public_errors(void) {
    struct ScMemOpsS *ops = start(); struct k2b_cedar_buffer b, saved; struct k2b_cedar_pin p, saved_pin; int calls = io_calls;
    memset(&b, 0x5a, sizeof(b)); saved = b; memset(&p, 0x5a, sizeof(p)); saved_pin = p;
    CHECK(k2b_cedar_memory_status(NULL) == -1 && errno == EINVAL);
    CHECK(k2b_cedar_memory_describe(NULL, &b) == -1 && !memcmp(&b, &saved, sizeof(b)));
    CHECK(k2b_cedar_memory_describe((void *)1, NULL) == -1);
    CHECK(k2b_cedar_memory_pin(NULL, &p) == -1 && !memcmp(&p, &saved_pin, sizeof(p)));
    CHECK(k2b_cedar_memory_pin((void *)1, NULL) == -1 && k2b_cedar_memory_unpin(NULL) == -1);
    CHECK(!status().error && io_calls == calls); finish(ops, 0);
}
static void test_pin(void) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(1, NULL, NULL); struct k2b_cedar_pin pin, stale, untouched;
    CHECK(k2b_cedar_memory_pin((char *)p + 7, &pin) == 0 && pin.buffer.offset == 7 && status().pinned == 1);
    memset(&untouched, 0xa5, sizeof(untouched)); stale = untouched;
    CHECK(k2b_cedar_memory_pin(p, &untouched) == -1 && errno == EBUSY && !memcmp(&untouched, &stale, sizeof(stale)));
    stale = pin; ++stale.token; CHECK(k2b_cedar_memory_unpin(&stale) == -1 && status().pinned == 1);
    stale = pin; ++stale.buffer.fd; CHECK(k2b_cedar_memory_unpin(&stale) == -1);
    CHECK(k2b_cedar_memory_unpin(&pin) == 0 && k2b_cedar_memory_unpin(&pin) == -1 && !status().error);
    CHECK(k2b_cedar_memory_pin(p, &stale) == 0 && stale.token > pin.token);
    ops->pfree(p, NULL, NULL); CHECK(status().error == EBUSY && status().allocations == 1 && !unmaps);
    CHECK(k2b_cedar_memory_unpin(&stale) == 0); ops->pfree(p, NULL, NULL); finish(ops, 1);
}
static void test_residual(void) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(1, NULL, NULL); ops->close();
    CHECK(status().references == 0 && status().allocations == 1 && !unmaps && !releases);
    CHECK(k2b_cedar_memory_end() == -1 && errno == EBUSY && !releases);
    ops->pfree(p, NULL, NULL); CHECK(k2b_cedar_memory_end() == 0); no_resources();
}
static void test_begin_fault(enum fault f) {
    int retained = f == VE_STAT || f == VE_TYPE || f == REQUEST_FAIL; int old_calls;
    fault = f; CHECK(k2b_cedar_memory_begin() == -1 && status().error > 0);
    CHECK(status().active == retained && !status().references && !status().allocations);
    CHECK(!model_open && heap_open == retained && ve_open == retained && !releases && !unmaps);
    CHECK(requests == (f == REQUEST_FAIL)); old_calls = io_calls;
    CHECK(k2b_cedar_memory_begin() == -1 && io_calls == old_calls);
    CHECK(k2b_cedar_memory_end() == -1 && io_calls == old_calls);
    if (!retained) no_resources();
}
static void test_alloc_fault(enum fault f) {
    struct ScMemOpsS *ops = start(); int count; fault = f; CHECK(!ops->palloc(4096, NULL, NULL));
    CHECK(status().error > 0); count = io_calls; CHECK(!ops->palloc(1, NULL, NULL) && io_calls == count);
    if (f == MAP_FAIL || f == ADDRESS_ZERO || f == ADDRESS_OVERFLOW) {
        CHECK(status().quarantined == 1 && status().allocations == 1 && status().live_bytes == 4096);
        CHECK(dma[0].open && dma[0].ptr && dma[0].mapped && !unmaps && !releases);
        CHECK(ops->get_fd_by_vir(dma[0].ptr) == -1 && !ops->ve_get_phyaddr(dma[0].ptr));
        ops->pfree(dma[0].ptr, NULL, NULL); CHECK(io_calls == count); ops->close();
        CHECK(k2b_cedar_memory_end() == -1 && !releases && status().active);
    } else { CHECK(!status().allocations && !unmaps); finish(ops, 1); }
}
static void test_free_fault(enum fault f) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(4096, NULL, NULL); int count; CHECK(p); fault = f;
    ops->pfree(p, NULL, NULL); CHECK(status().error == EIO && unmaps == 1);
    if (f == DMA_CLOSE) { CHECK(strstr(events, "UuD")); finish(ops, 1); return; }
    CHECK(status().allocations == 1 && status().quarantined == 1 && dma[0].open && dma[0].ptr);
    CHECK(dma[0].mapped == (f == UNMAP_FAIL)); count = io_calls;
    ops->pfree(p, NULL, NULL); CHECK(count == io_calls); ops->close(); CHECK(k2b_cedar_memory_end() == -1 && !releases);
}
static void test_end_fault(enum fault f) {
    struct ScMemOpsS *ops = start(); int count; ops->close(); fault = f;
    CHECK(k2b_cedar_memory_end() == -1 && status().error == EIO && releases == 1); count = io_calls;
    CHECK(k2b_cedar_memory_end() == -1 && count == io_calls && releases == 1);
    if (f == RELEASE_FAIL) CHECK(status().active && heap_open && ve_open);
    else no_resources();
}
static void test_bad_callback(int mode) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(4096, NULL, NULL); int count = io_calls;
    if (mode == 0) ops->pfree((char *)p + 1, NULL, NULL);
    if (mode == 1) ops->pfree((void *)1, NULL, NULL);
    if (mode == 2) ops->flush_cache(p, -1);
    if (mode == 3) ops->flush_cache((char *)p + 4095, 2);
    if (mode == 4) ops->flush_cache((void *)1, 1);
    if (mode == 5) { ops->close(); ops->close(); CHECK(status().error == EINVAL); ops->pfree(p, NULL, NULL); CHECK(k2b_cedar_memory_end() == -1); no_resources(); return; }
    CHECK(status().error == EINVAL && count == io_calls && status().allocations == 1);
    ops->pfree(p, NULL, NULL); finish(ops, 1);
}
static void test_limits(int mode) {
    struct ScMemOpsS *ops = start(); void *a = NULL, *b = NULL, *c = NULL; int calls;
    if (mode == 3) { a = ops->palloc(32*1024*1024, NULL, NULL); b = ops->palloc(32*1024*1024, NULL, NULL); c = ops->palloc(32*1024*1024, NULL, NULL); CHECK(a && b && c && status().live_bytes == 96U*1024*1024); }
    calls = alloc_calls; CHECK(!ops->palloc(mode == 0 ? 0 : mode == 1 ? -1 : mode == 2 ? 32*1024*1024+1 : 1, NULL, NULL));
    CHECK(alloc_calls == calls); ops->pfree(a, NULL, NULL); ops->pfree(b, NULL, NULL); ops->pfree(c, NULL, NULL); finish(ops, status().error != 0);
}
static void test_overlap(void) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(8192, NULL, NULL); CHECK(p); fault = ADDRESS_OVERLAP;
    CHECK(!ops->palloc(4096, NULL, NULL) && status().quarantined == 1 && status().allocations == 2);
    ops->pfree(p, NULL, NULL); CHECK(status().allocations == 1 && unmaps == 1 && dma[1].mapped && dma[1].open);
    ops->close(); CHECK(k2b_cedar_memory_end() == -1 && !releases);
}
static void test_flush_failure(void) { struct ScMemOpsS *ops = start(); void *p = ops->palloc(4096, NULL, NULL); fault = FLUSH_FAIL; ops->flush_cache(p, 4096); CHECK(status().error == EIO && flushes == 1); ops->pfree(p, NULL, NULL); finish(ops, 1); }
static void test_prebegin(int alloc) { int calls = io_calls; if (alloc) CHECK(!MemAdapterGetOpsS()->palloc(1, NULL, NULL)); else CHECK(MemAdapterGetOpsS()->open() == -1); CHECK(io_calls == calls && !status().active); no_resources(); }
static void test_unsupported(int mode) {
    struct ScMemOpsS *ops = start(); int calls = io_calls;
    if (mode == 0) CHECK(!ops->palloc_no_cache(1, NULL, NULL));
    if (mode == 1) CHECK(ops->get_vir_by_fd(1, NULL) == -1);
    if (mode == 2) CHECK(ops->get_phy_by_fd(1, NULL) == -1);
    if (mode == 3) CHECK(ops->free_phy_by_fd(1, 0) == -1);
    CHECK(errno == ENOTSUP && status().error == ENOTSUP && io_calls == calls); finish(ops, 1);
}
static void test_reconnect_tokens(void) {
    struct ScMemOpsS *ops = start(); struct k2b_cedar_pin first, second;
    void *p = ops->palloc(1, NULL, NULL); CHECK(p && k2b_cedar_memory_pin(p, &first) == 0);
    CHECK(k2b_cedar_memory_unpin(&first) == 0); ops->pfree(p, NULL, NULL); finish(ops, 0);
    ops = start(); p = ops->palloc(1, NULL, NULL); CHECK(p && k2b_cedar_memory_pin(p, &second) == 0);
    CHECK(second.token > first.token && k2b_cedar_memory_unpin(&first) == -1 && !status().error);
    CHECK(k2b_cedar_memory_unpin(&second) == 0); ops->pfree(p, NULL, NULL); finish(ops, 0);
}
static void test_first_error(void) {
    struct ScMemOpsS *ops = start(); void *p = ops->palloc(1, NULL, NULL); int calls;
    ops->flush_cache(p, -1); CHECK(status().error == EINVAL); fault = DMA_CLOSE;
    ops->pfree(p, NULL, NULL); CHECK(errno == EIO && status().error == EINVAL);
    finish(ops, 1); CHECK(status().error == EINVAL); calls = io_calls;
    CHECK(k2b_cedar_memory_begin() == -1 && errno == EINVAL && calls == io_calls);
}
static void *thread_use(void *unused) {
    struct ScMemOpsS *ops = MemAdapterGetOpsS(); struct k2b_cedar_pin pin; void *p; (void)unused;
    CHECK(ops->open2(NULL, NULL) == 0); p = ops->palloc(123, NULL, NULL); CHECK(p);
    CHECK(k2b_cedar_memory_pin(p, &pin) == 0); ops->flush_cache(p, 123);
    CHECK(k2b_cedar_memory_unpin(&pin) == 0); ops->pfree(p, NULL, NULL); ops->close(); return NULL;
}
static void test_threads(void) {
    pthread_t threads[4]; size_t i; CHECK(k2b_cedar_memory_begin() == 0);
    for (i = 0; i < 4; ++i) CHECK(pthread_create(&threads[i], NULL, thread_use, NULL) == 0);
    for (i = 0; i < 4; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(!status().error && !status().allocations && !status().references);
    CHECK(k2b_cedar_memory_end() == 0); no_resources();
}
static void run_case(int group, int value) {
    pid_t pid = fork(); int result; CHECK(pid >= 0);
    if (!pid) {
        alarm(10);
        switch (group) {
        case 0: test_normal(); break; case 1: test_fd_zero(); break; case 2: test_public_errors(); break;
        case 3: test_pin(); break; case 4: test_residual(); break;
        case 5: test_begin_fault((enum fault)value); break; case 6: test_alloc_fault((enum fault)value); break;
        case 7: test_free_fault((enum fault)value); break; case 8: test_end_fault((enum fault)value); break;
        case 9: test_bad_callback(value); break; case 10: test_limits(value); break; case 11: test_overlap(); break;
        case 12: test_flush_failure(); break; case 13: test_prebegin(value); break; case 14: test_unsupported(value); break;
        case 15: test_reconnect_tokens(); break; case 16: test_first_error(); break;
        case 17: test_threads(); break; default: CHECK(0);
        }
        exit(0);
    }
    CHECK(waitpid(pid, &result, 0) == pid);
    if (!WIFEXITED(result) || WEXITSTATUS(result)) { fprintf(stderr, "failed case group=%d value=%d\n", group, value); exit(1); }
    ++cases_run;
}
int main(void) {
    int i; alarm(120);
    for (i = 0; i < 5; ++i) run_case(i, 0);
    for (i = UNAME_FAIL; i <= REQUEST_FAIL; ++i) run_case(5, i);
    for (i = ALLOC_FAIL; i <= ADDRESS_OVERFLOW; ++i) run_case(6, i);
    for (i = UNMAP_FAIL; i <= DMA_CLOSE; ++i) run_case(7, i);
    run_case(8, RELEASE_FAIL); run_case(8, VE_CLOSE); run_case(8, HEAP_CLOSE);
    for (i = 0; i < 6; ++i) run_case(9, i);
    for (i = 0; i < 4; ++i) run_case(10, i);
    run_case(11, 0); run_case(12, 0); run_case(13, 0); run_case(13, 1);
    for (i = 0; i < 4; ++i) run_case(14, i);
    run_case(15, 0); run_case(16, 0); run_case(17, 0);
    printf("cedar memory tests passed (%u isolated cases)\n", cases_run); return 0;
}
