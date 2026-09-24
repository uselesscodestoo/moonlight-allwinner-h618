#ifndef K2B_CEDAR_RUNTIME_H
#define K2B_CEDAR_RUNTIME_H

#include <vdecoder.h>
#include "cedar_memory.h"

struct k2b_cedar_api {
    VideoDecoder *(*create)(void);
    void (*destroy)(VideoDecoder *);
    int (*initialize)(VideoDecoder *, VideoStreamInfo *, VConfig *);
    void (*reset)(VideoDecoder *);
    int (*decode)(VideoDecoder *, int, int, int, int64_t);
    int (*request_stream)(VideoDecoder *, int, char **, int *, char **, int *, int);
    int (*submit_stream)(VideoDecoder *, VideoStreamDataInfo *, int);
    int (*stream_frames)(VideoDecoder *, int);
    VideoPicture *(*request_picture)(VideoDecoder *, int);
    int (*return_picture)(VideoDecoder *, VideoPicture *);
    struct ScMemOpsS *(*mem_ops)(void);
    int (*memory_begin)(void);
    int (*memory_end)(void);
    int (*memory_status)(struct k2b_cedar_memory_status *);
    int (*memory_describe)(const void *, struct k2b_cedar_buffer *);
    int (*memory_pin)(const void *, struct k2b_cedar_pin *);
    int (*memory_unpin)(const struct k2b_cedar_pin *);
};

struct k2b_cedar_runtime_status {
    int ready, error;
    char detail[256];
};

/* Returns 0/-1 with errno; failures leave caller outputs untouched. Invalid
 * arguments do not initialize or poison the runtime. The first operational
 * failure is sticky. Success publishes an immutable, process-lifetime table
 * and registers H.264 exactly once, without creating a decoder or opening a
 * device. Handles and registration are never unloaded, including on failure.
 * Subsequent success requires the same canonical directory (otherwise EXDEV).
 * Set the environment before starting threads. Ordinary threads are serialized;
 * signal handlers, cancellation, and fork with an active runtime are unsupported.
 * Path/provider checks detect deployment mistakes, not hostile loader/file races.
 */
int k2b_cedar_runtime_load(const char *directory, const struct k2b_cedar_api **out);
int k2b_cedar_runtime_status(struct k2b_cedar_runtime_status *out);

#endif
