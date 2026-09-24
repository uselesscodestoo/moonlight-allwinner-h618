#ifndef K2B_CEDAR_MEMORY_H
#define K2B_CEDAR_MEMORY_H
#include <stddef.h>
#include <stdint.h>
struct ScMemOpsS;
/* Borrowed view: fd and base stay owned by the adapter. bytes is the whole
 * page-aligned allocation, offset locates the input pointer, and ve_address
 * is always the VE base (not the input pointer's translated address). */
struct k2b_cedar_buffer {
    const void *base;
    size_t bytes, offset;
    int fd;
    uint32_t ve_address;
};
/* A pin prevents pfree only. It does not prevent decoder picture reuse and
 * does not authorize ReturnPicture or imply display completion. One pin per
 * allocation; tokens are never reused, including across healthy sessions. */
struct k2b_cedar_pin { struct k2b_cedar_buffer buffer; uint64_t token; };
struct k2b_cedar_memory_status {
    size_t allocations, live_bytes, peak_bytes, pinned, quarantined;
    unsigned int references;
    int active, error;
};
/* One process-global session, serialized for ordinary threads. Not usable
 * from signal handlers, cancellation, or after fork with an active session.
 * All int APIs return 0/-1 with errno; failed queries leave outputs untouched.
 * First operational/callback error is sticky for the process lifetime. No
 * automatic reopen or forced cleanup exists. peak_bytes is process lifetime.
 * Begin precedes CedarC users; end requires all callbacks closed and every
 * allocation explicitly freed. Quarantined/unknown resources block end. */
int k2b_cedar_memory_begin(void);
int k2b_cedar_memory_end(void);
int k2b_cedar_memory_status(struct k2b_cedar_memory_status *out);
int k2b_cedar_memory_describe(const void *ptr, struct k2b_cedar_buffer *out);
int k2b_cedar_memory_pin(const void *ptr, struct k2b_cedar_pin *out);
int k2b_cedar_memory_unpin(const struct k2b_cedar_pin *pin);
/* ScMemOps open/close change references only; they never open/close devices.
 * Query views are unpinned and can become invalid after another thread frees
 * the allocation. Callers must coordinate their use or retain a pin. */
struct ScMemOpsS *MemAdapterGetOpsS(void);
struct ScMemOpsS *SecureMemAdapterGetOpsS(void);
int MemAdapterGetDramFreq(void);
#endif
