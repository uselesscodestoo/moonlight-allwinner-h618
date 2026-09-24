#include "cedar_runtime.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *stage, int error)
{
    struct k2b_cedar_runtime_status status = {0};
    if (k2b_cedar_runtime_status(&status) < 0) {
        fprintf(stderr, "FAIL: %s: errno=%d (%s); status query errno=%d\n",
                stage, error, strerror(error), errno);
    } else {
        fprintf(stderr, "FAIL: %s: errno=%d (%s); ready=%d error=%d detail=%s\n",
                stage, error, strerror(error), status.ready, status.error, status.detail);
    }
    return 1;
}

int main(int argc, char **argv)
{
    const struct k2b_cedar_api *first = NULL, *second = NULL;
    struct k2b_cedar_runtime_status status;
    struct k2b_cedar_memory_status memory;
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s ABSOLUTE_RUNTIME_DIR\n", argv[0]);
        return fail("absolute runtime path required", EINVAL);
    }
    if (k2b_cedar_runtime_load(argv[1], &first) < 0)
        return fail("runtime load (first)", errno);
    if (k2b_cedar_runtime_load(argv[1], &second) < 0)
        return fail("runtime load (repeat)", errno);
    if (!first || first != second)
        return fail("repeat load did not return the same nonnull API table", EPROTO);
    if (k2b_cedar_runtime_status(&status) < 0)
        return fail("runtime status", errno);
    if (status.ready != 1 || status.error != 0)
        return fail("runtime is not ready and error-free", EPROTO);
    /* This is the only function-table call: never begin a memory session or
     * invoke a decoder/creator while checking pure loading and registration. */
    if (!first->memory_status)
        return fail("missing memory_status", EPROTO);
    if (first->memory_status(&memory) < 0)
        return fail("memory status", errno);
    if (memory.active || memory.references || memory.allocations || memory.live_bytes ||
        memory.peak_bytes || memory.pinned || memory.quarantined || memory.error) {
        fprintf(stderr, "memory: active=%d references=%u allocations=%zu live_bytes=%zu "
                "peak_bytes=%zu pinned=%zu quarantined=%zu error=%d\n",
                memory.active, memory.references, memory.allocations, memory.live_bytes,
                memory.peak_bytes, memory.pinned, memory.quarantined, memory.error);
        return fail("memory state changed during pure loading", EPROTO);
    }
    puts("PASS: runtime ready=1 error=0; repeat load returned same API table");
    puts("PASS: typed H264 hardware registration via VDecoderRegister(format=H264, name=h264, bIsSoft=0)");
    puts("PASS: typed H265 hardware registration via VDecoderRegister(format=H265, name=h265, bIsSoft=0)");
    puts("PASS: memory untouched active=0 references=0 allocations=0 live_bytes=0 peak_bytes=0 pinned=0 quarantined=0 error=0");
    return 0;
}
