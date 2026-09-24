#include "input_queue.h"

#include <pthread.h>
#include <stdlib.h>

struct k2b_input_queue {
  pthread_mutex_t mutex;
  pthread_cond_t ready;
  unsigned char *storage;
  struct k2b_access_unit unit[K2B_INPUT_QUEUE_MAX_SLOTS];
  size_t slots, slot_bytes, head, tail, count, high_watermark;
  int leased, stopped;
  uint64_t next_token, active_token;
  uint64_t accepted, released, rejected_full, rejected_input;
  uint64_t rejected_stopped, discarded;
  unsigned int waiting;
};

/* Caller holds the mutex. Unlock errors never fabricate a successful result. */
static int unlock_result(struct k2b_input_queue *queue, int result)
{
  return pthread_mutex_unlock(&queue->mutex) == 0 ? result : K2B_QUEUE_ERROR;
}

static unsigned char *head_data(struct k2b_input_queue *queue)
{
  return queue->storage + queue->head * queue->slot_bytes;
}

/* Caller holds the mutex; a lease always occupies the head slot. */
static void discard_locked(struct k2b_input_queue *queue)
{
  size_t keep = queue->leased ? 1u : 0u;
  queue->discarded += queue->count - keep;
  queue->count = keep;
  queue->tail = (queue->head + keep) % queue->slots;
}

int k2b_input_queue_create(struct k2b_input_queue **out, size_t slots,
                           size_t slot_bytes)
{
  struct k2b_input_queue *queue;

  if (out == NULL || *out != NULL || slots == 0 || slots > K2B_INPUT_QUEUE_MAX_SLOTS ||
      slot_bytes == 0 || slot_bytes > K2B_AU_MAX_BYTES)
    return K2B_QUEUE_INVALID;

  queue = calloc(1, sizeof(*queue));
  if (queue == NULL)
    return K2B_QUEUE_ERROR;
  /* Validated bounds limit this multiplication to 32 MiB. */
  queue->storage = malloc(slots * slot_bytes);
  if (queue->storage == NULL) {
    free(queue);
    return K2B_QUEUE_ERROR;
  }
  if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
    free(queue->storage);
    free(queue);
    return K2B_QUEUE_ERROR;
  }
  if (pthread_cond_init(&queue->ready, NULL) != 0) {
    (void)pthread_mutex_destroy(&queue->mutex);
    free(queue->storage);
    free(queue);
    return K2B_QUEUE_ERROR;
  }
  queue->slots = slots;
  queue->slot_bytes = slot_bytes;
  *out = queue;
  return K2B_QUEUE_OK;
}

int k2b_input_queue_push(struct k2b_input_queue *queue, const DECODE_UNIT *unit)
{
  int result;
  if (queue == NULL)
    return K2B_QUEUE_INVALID;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  if (queue->stopped) {
    queue->rejected_stopped++;
    return unlock_result(queue, K2B_QUEUE_STOPPED);
  }
  if (queue->count == queue->slots) {
    queue->rejected_full++;
    return unlock_result(queue, K2B_QUEUE_FULL);
  }
  result = k2b_access_unit_copy(&queue->unit[queue->tail],
      queue->storage + queue->tail * queue->slot_bytes, queue->slot_bytes, unit);
  if (result != K2B_AU_OK) {
    queue->rejected_input++;
    return unlock_result(queue, result == K2B_AU_NO_SPACE ?
        K2B_QUEUE_TOO_LARGE : K2B_QUEUE_INVALID);
  }
  queue->tail = (queue->tail + 1) % queue->slots;
  queue->count++;
  queue->accepted++;
  if (queue->count > queue->high_watermark)
    queue->high_watermark = queue->count;
  (void)pthread_cond_signal(&queue->ready);
  return unlock_result(queue, K2B_QUEUE_OK);
}

int k2b_input_queue_take(struct k2b_input_queue *queue,
                         struct k2b_input_view *out, int wait)
{
  struct k2b_input_view view;
  if (queue == NULL || out == NULL || (wait != 0 && wait != 1))
    return K2B_QUEUE_INVALID;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  for (;;) {
    int result;
    if (queue->stopped)
      return unlock_result(queue, K2B_QUEUE_STOPPED);
    if (queue->leased)
      return unlock_result(queue, K2B_QUEUE_BUSY);
    if (queue->count != 0)
      break;
    if (!wait)
      return unlock_result(queue, K2B_QUEUE_EMPTY);
    queue->waiting++;
    result = pthread_cond_wait(&queue->ready, &queue->mutex);
    queue->waiting--;
    if (result != 0)
      return unlock_result(queue, K2B_QUEUE_ERROR);
    /* Recheck every predicate, including after a spurious wakeup. */
  }
  if (queue->next_token == UINT64_MAX)
    return unlock_result(queue, K2B_QUEUE_ERROR);
  queue->active_token = ++queue->next_token;
  queue->leased = 1;
  view.data = head_data(queue);
  view.unit = queue->unit[queue->head];
  view.owner = queue;
  view.token = queue->active_token;
  /* Publish only after successfully unlocking so ERROR preserves *out. */
  if (unlock_result(queue, K2B_QUEUE_OK) != K2B_QUEUE_OK)
    return K2B_QUEUE_ERROR;
  *out = view;
  return K2B_QUEUE_OK;
}

int k2b_input_queue_release(struct k2b_input_queue *queue,
                            const struct k2b_input_view *view)
{
  if (queue == NULL || view == NULL)
    return K2B_QUEUE_INVALID;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  if (!queue->leased || view->owner != queue ||
      view->token != queue->active_token || view->data != head_data(queue))
    return unlock_result(queue, K2B_QUEUE_INVALID);
  queue->head = (queue->head + 1) % queue->slots;
  queue->count--;
  queue->leased = 0;
  queue->released++;
  return unlock_result(queue, K2B_QUEUE_OK);
}

int k2b_input_queue_discard_pending(struct k2b_input_queue *queue)
{
  if (queue == NULL)
    return K2B_QUEUE_INVALID;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  discard_locked(queue);
  return unlock_result(queue, K2B_QUEUE_OK);
}

int k2b_input_queue_stop(struct k2b_input_queue *queue)
{
  if (queue == NULL)
    return K2B_QUEUE_INVALID;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  queue->stopped = 1;
  discard_locked(queue);
  (void)pthread_cond_broadcast(&queue->ready);
  return unlock_result(queue, K2B_QUEUE_OK);
}

int k2b_input_queue_stats(struct k2b_input_queue *queue,
                          struct k2b_input_stats *out)
{
  struct k2b_input_stats stats;
  if (queue == NULL || out == NULL)
    return K2B_QUEUE_INVALID;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  stats.accepted = queue->accepted;
  stats.released = queue->released;
  stats.rejected_full = queue->rejected_full;
  stats.rejected_input = queue->rejected_input;
  stats.rejected_stopped = queue->rejected_stopped;
  stats.discarded = queue->discarded;
  stats.slots = queue->slots;
  stats.slot_bytes = queue->slot_bytes;
  stats.reserved_bytes = queue->slots * queue->slot_bytes;
  stats.occupied = queue->count;
  stats.queued = queue->count - (size_t)queue->leased;
  stats.high_watermark = queue->high_watermark;
  stats.waiting = queue->waiting;
  stats.leased = queue->leased;
  stats.stopped = queue->stopped;
  if (unlock_result(queue, K2B_QUEUE_OK) != K2B_QUEUE_OK)
    return K2B_QUEUE_ERROR;
  *out = stats;
  return K2B_QUEUE_OK;
}

int k2b_input_queue_destroy(struct k2b_input_queue **out)
{
  struct k2b_input_queue *queue;
  if (out == NULL || *out == NULL)
    return K2B_QUEUE_INVALID;
  queue = *out;
  if (pthread_mutex_lock(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  if (!queue->stopped || queue->leased || queue->waiting != 0)
    return unlock_result(queue, K2B_QUEUE_BUSY);
  if (unlock_result(queue, K2B_QUEUE_OK) != K2B_QUEUE_OK)
    return K2B_QUEUE_ERROR;
  /* The caller has joined/quiesced everyone; no synchronization object can
   * still be in use between this check and teardown. */
  if (pthread_cond_destroy(&queue->ready) != 0)
    return K2B_QUEUE_ERROR;
  if (pthread_mutex_destroy(&queue->mutex) != 0)
    return K2B_QUEUE_ERROR;
  free(queue->storage);
  free(queue);
  *out = NULL;
  return K2B_QUEUE_OK;
}
