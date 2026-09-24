#ifndef K2B_INPUT_QUEUE_H
#define K2B_INPUT_QUEUE_H

#include "access_unit.h"

#define K2B_INPUT_QUEUE_MAX_SLOTS 8u

struct k2b_input_queue;

enum k2b_queue_result {
  K2B_QUEUE_OK = 0,
  K2B_QUEUE_EMPTY = 1,
  K2B_QUEUE_FULL = 2,
  K2B_QUEUE_STOPPED = 3,
  K2B_QUEUE_BUSY = 4,
  K2B_QUEUE_TOO_LARGE = 5,
  K2B_QUEUE_INVALID = -1,
  K2B_QUEUE_ERROR = -2
};

struct k2b_input_view {
  const unsigned char *data;
  struct k2b_access_unit unit;
  const struct k2b_input_queue *owner;
  uint64_t token;
};

struct k2b_input_stats {
  uint64_t accepted, released, rejected_full, rejected_input;
  uint64_t rejected_stopped, discarded;
  size_t slots, slot_bytes, reserved_bytes, occupied, queued, high_watermark;
  unsigned int waiting;
  int leased, stopped;
};

/* Single producer/single consumer, with concurrent control and stats calls.
 * All supplied non-NULL pointers must be valid; outputs must not overlap any
 * queue or source storage. No signal-handler calls or pthread cancellation.
 * Source units/chains/data remain stable for the complete push call.
 * Push/take/release allocate nothing. All shared state is mutex protected. */

/* Requires out != NULL and *out == NULL; slots is 1..K2B_INPUT_QUEUE_MAX_SLOTS,
 * slot_bytes is 1..K2B_AU_MAX_BYTES. Preallocates at most 32 MiB of slot data.
 * Failure leaves *out unchanged: INVALID parameters, ERROR resources. */
int k2b_input_queue_create(struct k2b_input_queue **out, size_t slots,
                           size_t slot_bytes);

/* Never waits for space. STOPPED precedes FULL, both without inspecting unit.
 * Other input errors leave queued/leased bytes and metadata unchanged. */
int k2b_input_queue_push(struct k2b_input_queue *queue, const DECODE_UNIT *unit);

/* wait must be 0 or 1. STOPPED precedes BUSY (an outstanding lease).
 * wait == 1 waits for data or stop; otherwise an empty queue returns EMPTY.
 * Only success changes *out. Tokens never wrap; exhaustion returns ERROR.
 * Occupancy includes the lease. Keep the returned view and bytes immutable
 * until release; only one lease can exist at a time. */
int k2b_input_queue_take(struct k2b_input_queue *queue,
                         struct k2b_input_view *out, int wait);

/* Validates owner, token and data; stale/duplicate/foreign views are INVALID.
 * Allowed after stop. Ends compressed-slot lifetime only: this is unrelated
 * to VPU frame ownership or DMA/display retirement. */
int k2b_input_queue_release(struct k2b_input_queue *queue,
                            const struct k2b_input_view *view);

/* Discards queued inputs while preserving any lease. Idempotent, including
 * after stop. Dropped input/reference-chain recovery belongs to the caller. */
int k2b_input_queue_discard_pending(struct k2b_input_queue *queue);

/* Idempotently stops push/take, discards pending inputs and wakes waiters.
 * A live lease remains readable and releasable. */
int k2b_input_queue_stop(struct k2b_input_queue *queue);

/* Locked snapshot. Ordinary uint64_t counters may wrap; tokens do not. */
int k2b_input_queue_stats(struct k2b_input_queue *queue,
                          struct k2b_input_stats *out);

/* Caller MUST quiesce all other calls and join all threads first. This is not
 * a concurrent cancellation API. Requires stopped, no lease and no waiters
 * (otherwise BUSY). Success frees storage and sets *queue to NULL. NULL
 * queue or *queue is INVALID. No access after successful destroy is valid. */
int k2b_input_queue_destroy(struct k2b_input_queue **queue);

#endif
