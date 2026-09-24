/* Explicit K2B/vendor-5.4 session. Unknown kernel side effects are quarantined;
 * there is deliberately no reset, forced cleanup or automatic reopen path. */
#define _GNU_SOURCE
#include "cedar_memory.h"
#include "cedar_uapi.h"
#include <sc_interface.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

/* The callback table crosses a prebuilt library boundary. Designated
 * initializers check types, but cannot detect same-type slots being swapped. */
#define MEMOPS_SLOT(field, index) typedef char k2b_memops_slot_##field[ \
    offsetof(struct ScMemOpsS, field) == (index) * 8U ? 1 : -1]
MEMOPS_SLOT(open, 0);
MEMOPS_SLOT(open2, 1);
MEMOPS_SLOT(close, 2);
MEMOPS_SLOT(total_size, 3);
MEMOPS_SLOT(palloc, 4);
MEMOPS_SLOT(palloc_no_cache, 5);
MEMOPS_SLOT(pfree, 6);
MEMOPS_SLOT(flush_cache, 7);
MEMOPS_SLOT(ve_get_phyaddr, 8);
MEMOPS_SLOT(ve_get_viraddr, 9);
MEMOPS_SLOT(cpu_get_phyaddr, 10);
MEMOPS_SLOT(cpu_get_viraddr, 11);
MEMOPS_SLOT(mem_set, 12);
MEMOPS_SLOT(mem_cpy, 13);
MEMOPS_SLOT(mem_read, 14);
MEMOPS_SLOT(mem_write, 15);
MEMOPS_SLOT(setup, 16);
MEMOPS_SLOT(shutdown, 17);
MEMOPS_SLOT(get_ve_addr_offset, 18);
MEMOPS_SLOT(get_debug_info, 19);
MEMOPS_SLOT(get_vir_by_fd, 20);
MEMOPS_SLOT(get_phy_by_fd, 21);
MEMOPS_SLOT(free_phy_by_fd, 22);
MEMOPS_SLOT(get_fd_by_vir, 23);
#undef MEMOPS_SLOT
typedef char k2b_memops_size[sizeof(struct ScMemOpsS) == 192 ? 1 : -1];

#define MAX_ALLOCATION (32U * 1024U * 1024U)
#define MAX_LIVE (96U * 1024U * 1024U)
struct allocation {
    struct allocation *next;
    void *base;
    size_t bytes;
    struct dma_buf_param map;
    uint64_t pin;
    int quarantined;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct allocation *allocations;
static struct k2b_cedar_memory_status state;
static int heap_fd = -1, ve_fd = -1, engine_requested;
static uint64_t last_token;

static int reject(int error) { errno = error; return -1; }
/* Call with lock held. Retain the first cause, but return the current errno. */
static int record(int error)
{
    if (error <= 0) error = EIO;
    if (!state.error) state.error = error;
    return reject(error);
}
static int healthy(void)
{
    if (state.error) return reject(state.error);
    if (!state.active || !engine_requested) return reject(ENODEV);
    return 0;
}
static struct allocation *lookup(const void *ptr)
{
    struct allocation *a;
    uintptr_t value = (uintptr_t)ptr;
    for (a = allocations; a; a = a->next)
        if (value >= (uintptr_t)a->base && value - (uintptr_t)a->base < a->bytes)
            return a;
    return NULL;
}
static void describe(struct allocation *a, const void *ptr, struct k2b_cedar_buffer *out)
{
    out->base = a->base;
    out->bytes = a->bytes;
    out->offset = (uintptr_t)ptr - (uintptr_t)a->base;
    out->fd = a->map.fd;
    out->ve_address = a->map.phy_addr;
}

/* Identity is checked before touching either device. Read through EOF so an
 * otherwise correct prefix cannot hide truncation or extra model bytes. */
static int validate_identity(void)
{
    static const char expected[] = "KICKPI K2B";
    char model[sizeof(expected) + 1];
    struct utsname u;
    size_t used = 0;
    ssize_t count;
    long page;
    int fd, error = 0;
    if (uname(&u) < 0) return errno;
    if (strcmp(u.release, "5.4.125") || strcmp(u.machine, "aarch64")) return ENODEV;
    errno = 0;
    page = sysconf(_SC_PAGESIZE);
    if (page != 4096) return page < 0 && errno ? errno : ENODEV;
    fd = open("/proc/device-tree/model", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    while (used < sizeof(model)) {
        count = read(fd, model + used, sizeof(model) - used);
        if (count < 0) { error = errno; break; }
        if (!count) break;
        used += (size_t)count;
    }
    if (!error && !((used == sizeof(expected) - 1 ||
        (used == sizeof(expected) && model[used - 1] == '\0')) &&
        !memcmp(model, expected, sizeof(expected) - 1))) error = ENODEV;
    if (close(fd) < 0 && !error) error = errno;
    return error;
}

int k2b_cedar_memory_begin(void)
{
    struct stat st;
    int result = -1, error;
    pthread_mutex_lock(&lock);
    if (state.active) { reject(EBUSY); goto out; }
    if (state.error) { reject(state.error); goto out; }
    error = validate_identity();
    if (error) { record(error); goto out; }
    heap_fd = open("/dev/cedar_test_heap", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) { record(errno); goto out; }
    if (fstat(heap_fd, &st) < 0) { record(errno); goto safe_heap; }
    if (!S_ISCHR(st.st_mode)) { record(ENODEV); goto safe_heap; }
    ve_fd = open("/dev/cedar_dev", O_RDWR | O_CLOEXEC);
    if (ve_fd < 0) { record(errno); goto safe_heap; }
    /* Vendor release traverses the global DMA list even if ENGINE_REQ did
     * not initialize it. Once open succeeds no speculative close is safe. */
    state.active = 1;
    engine_requested = 0;
    if (fstat(ve_fd, &st) < 0) { record(errno); goto out; }
    if (!S_ISCHR(st.st_mode)) { record(ENODEV); goto out; }
    if (ioctl(ve_fd, IOCTL_ENGINE_REQ, 0UL) < 0) { record(errno); goto out; }
    engine_requested = 1;
    result = 0;
    goto out;
safe_heap:
    error = errno;
    if (close(heap_fd) < 0) record(errno);
    heap_fd = -1;
    errno = error;
out:
    pthread_mutex_unlock(&lock);
    return result;
}

int k2b_cedar_memory_end(void)
{
    int result = -1;
    pthread_mutex_lock(&lock);
    if (!state.active) { reject(EINVAL); goto out; }
    if (state.references || state.allocations) { reject(EBUSY); goto out; }
    if (!engine_requested) { reject(state.error ? state.error : EIO); goto out; }
    /* Consume permission to REL before issuing it: a failed REL may already
     * have decremented the kernel reference, so it must never be retried. */
    engine_requested = 0;
    if (ioctl(ve_fd, IOCTL_ENGINE_REL, 0UL) < 0) { record(errno); goto out; }
    if (close(ve_fd) < 0) record(errno);
    ve_fd = -1;
    if (close(heap_fd) < 0) record(errno);
    heap_fd = -1;
    state.active = 0;
    result = state.error ? reject(state.error) : 0;
out:
    pthread_mutex_unlock(&lock);
    return result;
}

int k2b_cedar_memory_status(struct k2b_cedar_memory_status *out)
{
    if (!out) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    *out = state;
    pthread_mutex_unlock(&lock);
    return 0;
}
int k2b_cedar_memory_describe(const void *ptr, struct k2b_cedar_buffer *out)
{
    struct allocation *a;
    int result = -1;
    if (!ptr || !out) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    a = lookup(ptr);
    if (!a || a->quarantined) reject(EINVAL);
    else { describe(a, ptr, out); result = 0; }
    pthread_mutex_unlock(&lock);
    return result;
}
int k2b_cedar_memory_pin(const void *ptr, struct k2b_cedar_pin *out)
{
    struct allocation *a;
    int result = -1;
    if (!ptr || !out) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    if (healthy() < 0) goto done;
    a = lookup(ptr);
    if (!a || a->quarantined) { reject(EINVAL); goto done; }
    if (a->pin) { reject(EBUSY); goto done; }
    if (last_token == UINT64_MAX) { record(EOVERFLOW); goto done; }
    a->pin = ++last_token;
    ++state.pinned;
    describe(a, ptr, &out->buffer);
    out->token = a->pin;
    result = 0;
done:
    pthread_mutex_unlock(&lock);
    return result;
}
int k2b_cedar_memory_unpin(const struct k2b_cedar_pin *pin)
{
    struct allocation *a;
    int result = -1;
    if (!pin) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    a = lookup(pin->buffer.base);
    if (!a || a->base != pin->buffer.base || !pin->token || a->pin != pin->token ||
        a->map.fd != pin->buffer.fd) reject(EINVAL);
    else { a->pin = 0; --state.pinned; result = 0; }
    pthread_mutex_unlock(&lock);
    return result;
}

static int mem_open(void)
{
    int result = -1;
    pthread_mutex_lock(&lock);
    if (healthy() < 0) record(errno);
    else if (state.references == UINT_MAX) record(EOVERFLOW);
    else { ++state.references; result = 0; }
    pthread_mutex_unlock(&lock);
    return result;
}
static int mem_open2(void *veops, void *veself)
{
    (void)veops; (void)veself;
    return mem_open();
}
static void mem_close(void)
{
    pthread_mutex_lock(&lock);
    if (!state.references) record(EINVAL);
    else --state.references;
    pthread_mutex_unlock(&lock);
}
static void quarantine(struct allocation *a)
{
    if (!a->quarantined) { a->quarantined = 1; ++state.quarantined; }
}
static void register_allocation(struct allocation *a)
{
    a->next = allocations;
    allocations = a;
    ++state.allocations;
    state.live_bytes += a->bytes;
    if (state.live_bytes > state.peak_bytes) state.peak_bytes = state.live_bytes;
}
static void *mem_alloc(int bytes, void *veops, void *veself)
{
    struct allocation *a, *other;
    size_t aligned;
    uint64_t end;
    void *result = NULL;
    int error;
    (void)veops; (void)veself;
    pthread_mutex_lock(&lock);
    if (healthy() < 0) { record(errno); goto out; }
    if (!state.references) { record(ENODEV); goto out; }
    if (bytes <= 0 || (unsigned int)bytes > MAX_ALLOCATION) { record(EINVAL); goto out; }
    aligned = ((size_t)bytes + 4095U) & ~(size_t)4095U;
    if (aligned > MAX_LIVE - state.live_bytes) { record(ENOMEM); goto out; }
    a = calloc(1, sizeof(*a));
    if (!a) { record(ENOMEM); goto out; }
    a->bytes = aligned;
    a->map.fd = ioctl(heap_fd, K2B_CEDAR_HEAP_ALLOC, (unsigned long)aligned);
    if (a->map.fd < 0) { record(errno); free(a); goto out; }
    a->base = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, a->map.fd, 0);
    if (a->base == MAP_FAILED) {
        error = errno;
        record(error);
        if (close(a->map.fd) < 0) record(errno);
        free(a);
        errno = error;
        goto out;
    }
    /* Record before MAP: even an error can mean successful registration with
     * failed copy_to_user. That allocation must block end and remain owned. */
    register_allocation(a);
    if (ioctl(ve_fd, IOCTL_MAP_DMA_BUF, &a->map) < 0) {
        record(errno); quarantine(a); goto out;
    }
    end = (uint64_t)a->map.phy_addr + a->bytes;
    if (!a->base || !a->map.phy_addr || end - 1 > UINT32_MAX) {
        record(EOVERFLOW); quarantine(a); goto out;
    }
    for (other = a->next; other; other = other->next) {
        if ((uint64_t)a->map.phy_addr < (uint64_t)other->map.phy_addr + other->bytes &&
            (uint64_t)other->map.phy_addr < end) {
            record(EINVAL); quarantine(a); goto out;
        }
    }
    result = a->base;
out:
    pthread_mutex_unlock(&lock);
    return result;
}
static void mem_free(void *ptr, void *veops, void *veself)
{
    struct allocation **slot, *a;
    (void)veops; (void)veself;
    if (!ptr) return;
    pthread_mutex_lock(&lock);
    for (slot = &allocations; *slot && (*slot)->base != ptr; slot = &(*slot)->next) {}
    a = *slot;
    if (!a) { record(EINVAL); goto out; }
    if (a->pin) { record(EBUSY); goto out; }
    if (a->quarantined) { reject(state.error ? state.error : EIO); goto out; }
    if (ioctl(ve_fd, IOCTL_UNMAP_DMA_BUF, &a->map) < 0) {
        record(errno); quarantine(a); goto out;
    }
    if (munmap(a->base, a->bytes) < 0) {
        record(errno); quarantine(a); goto out;
    }
    /* On Linux even an error consumes the close attempt. Never reuse/retry fd. */
    if (close(a->map.fd) < 0) record(errno);
    *slot = a->next;
    --state.allocations;
    state.live_bytes -= a->bytes;
    free(a);
out:
    pthread_mutex_unlock(&lock);
}
static void *virt_to_dma(void *ptr)
{
    struct allocation *a;
    void *result = NULL;
    uintptr_t offset;
    pthread_mutex_lock(&lock);
    a = lookup(ptr);
    if (a && !a->quarantined) {
        offset = (uintptr_t)ptr - (uintptr_t)a->base;
        if (offset <= UINT32_MAX - a->map.phy_addr)
            result = (void *)((uintptr_t)a->map.phy_addr + offset);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static void *dma_to_virt(void *ptr)
{
    struct allocation *a;
    uintptr_t value = (uintptr_t)ptr, offset;
    void *result = NULL;
    pthread_mutex_lock(&lock);
    for (a = allocations; a; a = a->next) {
        if (!a->quarantined && value >= a->map.phy_addr && value - a->map.phy_addr < a->bytes) {
            offset = value - a->map.phy_addr;
            if (offset <= UINTPTR_MAX - (uintptr_t)a->base) result = (void *)((uintptr_t)a->base + offset);
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static void flush_cache(void *ptr, int bytes)
{
    struct allocation *a;
    struct cache_range range;
    uintptr_t offset;
    if (!bytes) return;
    pthread_mutex_lock(&lock);
    a = lookup(ptr);
    if (bytes < 0 || !a || a->quarantined) { record(EINVAL); goto out; }
    offset = (uintptr_t)ptr - (uintptr_t)a->base;
    if ((size_t)bytes > a->bytes - offset || (uintptr_t)bytes > UINTPTR_MAX - (uintptr_t)ptr) {
        record(EINVAL); goto out;
    }
    if (healthy() < 0) goto out;
    range.start = (uintptr_t)ptr;
    range.end = range.start + (size_t)bytes;
    if (ioctl(ve_fd, IOCTL_FLUSH_CACHE_RANGE, &range) < 0) record(errno);
out:
    pthread_mutex_unlock(&lock);
}
static int get_fd(void *ptr)
{
    struct allocation *a;
    int result = -1;
    pthread_mutex_lock(&lock);
    a = lookup(ptr);
    if (a && !a->quarantined) result = a->map.fd;
    else reject(EINVAL);
    pthread_mutex_unlock(&lock);
    return result;
}
static int debug_info(char *dest, int bytes)
{
    int result;
    if (!dest || bytes <= 0) return reject(EINVAL);
    pthread_mutex_lock(&lock);
    result = snprintf(dest, (size_t)bytes, "active=%d refs=%u allocations=%zu live=%zu peak=%zu pinned=%zu quarantined=%zu error=%d",
        state.active, state.references, state.allocations, state.live_bytes, state.peak_bytes,
        state.pinned, state.quarantined, state.error);
    if (result >= bytes) result = bytes - 1;
    pthread_mutex_unlock(&lock);
    return result;
}
static int unsupported(void)
{
    pthread_mutex_lock(&lock);
    record(ENOTSUP);
    pthread_mutex_unlock(&lock);
    return -1;
}
static void *no_cache(int bytes, void *veops, void *veself)
{
    (void)bytes; (void)veops; (void)veself;
    unsupported(); return NULL;
}
static int import_fd(int fd, void *address) { (void)fd; (void)address; return unsupported(); }
static int free_import(int fd, unsigned long address) { (void)fd; (void)address; return unsupported(); }
static int mem_set(void *dest, int value, size_t bytes)
{
    pthread_mutex_lock(&lock); memset(dest, value, bytes); pthread_mutex_unlock(&lock); return 0;
}
static int mem_copy(void *dest, void *src, size_t bytes)
{
    pthread_mutex_lock(&lock); memcpy(dest, src, bytes); pthread_mutex_unlock(&lock); return 0;
}
static int no_resources(void) { return 0; }
static int total_size(void) { return MAX_LIVE / 1024; }
static unsigned int address_offset(void) { return 0; }
static struct ScMemOpsS ops = {
    .open = mem_open, .open2 = mem_open2, .close = mem_close,
    .total_size = total_size, .palloc = mem_alloc, .palloc_no_cache = no_cache,
    .pfree = mem_free, .flush_cache = flush_cache,
    .ve_get_phyaddr = virt_to_dma, .ve_get_viraddr = dma_to_virt,
    .cpu_get_phyaddr = virt_to_dma, .cpu_get_viraddr = dma_to_virt,
    .mem_set = mem_set, .mem_cpy = mem_copy, .mem_read = mem_copy, .mem_write = mem_copy,
    .setup = no_resources, .shutdown = no_resources, .get_ve_addr_offset = address_offset,
    .get_debug_info = debug_info, .get_vir_by_fd = import_fd, .get_phy_by_fd = import_fd,
    .free_phy_by_fd = free_import, .get_fd_by_vir = get_fd
};
struct ScMemOpsS *MemAdapterGetOpsS(void) { return &ops; }
struct ScMemOpsS *SecureMemAdapterGetOpsS(void) { return NULL; }
int MemAdapterGetDramFreq(void) { return -1; }
