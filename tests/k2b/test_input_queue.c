#define _POSIX_C_SOURCE 200809L
#include "input_queue.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
  exit(1); \
} } while (0)

static DECODE_UNIT make_unit(LENTRY *entry, unsigned char *bytes, size_t length,
                             int frame)
{
  DECODE_UNIT unit = { 0 };
  memset(entry, 0, sizeof(*entry));
  entry->data = (char *)bytes;
  entry->length = (int)length;
  entry->bufferType = BUFFER_TYPE_PICDATA;
  unit.bufferList = entry;
  unit.fullLength = (int)length;
  unit.frameNumber = frame;
  unit.frameType = FRAME_TYPE_IDR;
  unit.colorspace = COLORSPACE_REC_709;
  unit.presentationTimeUs = (uint64_t)frame * 1000u;
  unit.receiveTimeUs = (uint64_t)frame * 1000u + 1u;
  unit.enqueueTimeUs = (uint64_t)frame * 1000u + 2u;
  return unit;
}

static struct k2b_input_stats snapshot(struct k2b_input_queue *queue)
{
  struct k2b_input_stats stats;
  CHECK(k2b_input_queue_stats(queue, &stats) == K2B_QUEUE_OK);
  CHECK(stats.occupied <= stats.slots);
  CHECK(stats.queued + (size_t)stats.leased == stats.occupied);
  CHECK(stats.high_watermark <= stats.slots);
  return stats;
}

static void finish(struct k2b_input_queue **queue)
{
  CHECK(k2b_input_queue_stop(*queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_destroy(queue) == K2B_QUEUE_OK);
  CHECK(*queue == NULL);
}

static void test_lifecycle(void)
{
  struct k2b_input_queue *queue = NULL;
  struct k2b_input_view view;
  unsigned char bytes[] = { 1, 2, 3 };
  LENTRY entry = { 0 };
  DECODE_UNIT unit = { 0 };
  entry.data = (char *)bytes;
  entry.length = sizeof(bytes);
  entry.bufferType = BUFFER_TYPE_PICDATA;
  unit.bufferList = &entry;
  unit.fullLength = sizeof(bytes);
  unit.frameType = FRAME_TYPE_IDR;
  unit.colorspace = COLORSPACE_REC_709;
  CHECK(k2b_input_queue_create(&queue, 2, 16) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &view, 0) == K2B_QUEUE_OK);
  CHECK(view.unit.bytes == sizeof(bytes));
  CHECK(memcmp(view.data, bytes, sizeof(bytes)) == 0);
  CHECK(k2b_input_queue_release(queue, &view) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_stop(queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_OK);
  CHECK(queue == NULL);
}

static void test_parameters(void)
{
  struct k2b_input_queue *queue = NULL;
  struct k2b_input_queue *original;
  struct k2b_input_view view;
  struct k2b_input_view poison;
  struct k2b_input_stats stats;
  CHECK(k2b_input_queue_create(NULL, 1, 1) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_create(&queue, 0, 1) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_create(&queue, K2B_INPUT_QUEUE_MAX_SLOTS + 1, 1) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_create(&queue, 1, 0) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_create(&queue, 1, K2B_AU_MAX_BYTES + 1) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_create(&queue, SIZE_MAX, SIZE_MAX) == K2B_QUEUE_INVALID);
  CHECK(queue == NULL);
  CHECK(k2b_input_queue_push(NULL, NULL) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_take(NULL, &view, 0) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_release(NULL, &view) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_stop(NULL) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_discard_pending(NULL) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_stats(NULL, &stats) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_destroy(NULL) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_create(&queue, K2B_INPUT_QUEUE_MAX_SLOTS, K2B_AU_MAX_BYTES) == K2B_QUEUE_OK);
  original = queue;
  CHECK(k2b_input_queue_create(&queue, 1, 1) == K2B_QUEUE_INVALID);
  CHECK(queue == original);
  stats = snapshot(queue);
  CHECK(stats.slots == K2B_INPUT_QUEUE_MAX_SLOTS);
  CHECK(stats.slot_bytes == K2B_AU_MAX_BYTES);
  CHECK(stats.reserved_bytes == 32u * 1024u * 1024u);
  CHECK(stats.accepted == 0 && stats.released == 0 && stats.discarded == 0);
  CHECK(stats.rejected_full == 0 && stats.rejected_input == 0 && stats.rejected_stopped == 0);
  CHECK(stats.occupied == 0 && stats.queued == 0 && stats.high_watermark == 0);
  CHECK(stats.leased == 0 && stats.stopped == 0 && stats.waiting == 0);
  CHECK(k2b_input_queue_take(queue, NULL, 0) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_release(queue, NULL) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_stats(queue, NULL) == K2B_QUEUE_INVALID);
  memset(&view, 0xa5, sizeof(view));
  memcpy(&poison, &view, sizeof(view));
  CHECK(k2b_input_queue_take(queue, &view, -1) == K2B_QUEUE_INVALID);
  CHECK(memcmp(&view, &poison, sizeof(view)) == 0);
  CHECK(k2b_input_queue_take(queue, &view, 2) == K2B_QUEUE_INVALID);
  CHECK(memcmp(&view, &poison, sizeof(view)) == 0);
  CHECK(k2b_input_queue_take(queue, &view, 0) == K2B_QUEUE_EMPTY);
  CHECK(memcmp(&view, &poison, sizeof(view)) == 0);
  CHECK(k2b_input_queue_release(queue, &view) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_BUSY);
  CHECK(queue == original);
  CHECK(k2b_input_queue_push(queue, NULL) == K2B_QUEUE_INVALID);
  stats = snapshot(queue);
  CHECK(stats.rejected_input == 1 && stats.occupied == 0);
  finish(&queue);
  CHECK(k2b_input_queue_create(&queue, 1, 1) == K2B_QUEUE_OK);
  finish(&queue);
}

static void test_fifo(void)
{
  struct k2b_input_queue *queue = NULL;
  struct k2b_input_view view;
  struct k2b_input_view poison;
  struct k2b_input_stats stats;
  unsigned char bytes[4];
  LENTRY entry;
  DECODE_UNIT unit;
  uint64_t token = 0;
  int frame;
  CHECK(k2b_input_queue_create(&queue, 3, sizeof(bytes)) == K2B_QUEUE_OK);
  for (frame = 1; frame <= 30; frame++) {
    memset(bytes, frame, sizeof(bytes));
    unit = make_unit(&entry, bytes, sizeof(bytes), frame);
    CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
    memset(bytes, 0xee, sizeof(bytes));
    unit.frameNumber = -1;
    if (frame % 3 != 0)
      continue;
    stats = snapshot(queue);
    CHECK(stats.occupied == 3 && stats.queued == 3 && stats.high_watermark == 3);
    /* A full push must not inspect even a NULL unit. */
    CHECK(k2b_input_queue_push(queue, NULL) == K2B_QUEUE_FULL);
    {
      int expected;
      for (expected = frame - 2; expected <= frame; expected++) {
        CHECK(k2b_input_queue_take(queue, &view, 0) == K2B_QUEUE_OK);
        CHECK(view.owner == queue && view.token > token);
        token = view.token;
        CHECK(view.unit.frame_number == expected && view.unit.bytes == sizeof(bytes));
        CHECK(view.unit.pts_us == (int64_t)expected * 1000);
        CHECK(view.unit.receive_us == (uint64_t)expected * 1000 + 1);
        CHECK(view.unit.enqueue_us == (uint64_t)expected * 1000 + 2);
        CHECK(view.unit.frame_type == FRAME_TYPE_IDR && view.unit.colorspace == COLORSPACE_REC_709);
        memset(bytes, expected, sizeof(bytes));
        CHECK(memcmp(view.data, bytes, sizeof(bytes)) == 0);
        memset(&poison, 0xa5, sizeof(poison));
        CHECK(k2b_input_queue_take(queue, &poison, 0) == K2B_QUEUE_BUSY);
        {
          struct k2b_input_view unchanged;
          memset(&unchanged, 0xa5, sizeof(unchanged));
          CHECK(memcmp(&poison, &unchanged, sizeof(poison)) == 0);
        }
        stats = snapshot(queue);
        CHECK(stats.leased == 1 && stats.occupied == (size_t)(frame - expected + 1));
        CHECK(stats.queued == (size_t)(frame - expected));
        if (expected == frame - 2) {
          CHECK(k2b_input_queue_push(queue, NULL) == K2B_QUEUE_FULL);
          CHECK(memcmp(view.data, bytes, sizeof(bytes)) == 0);
        }
        CHECK(k2b_input_queue_release(queue, &view) == K2B_QUEUE_OK);
      }
    }
  }
  stats = snapshot(queue);
  CHECK(stats.accepted == 30 && stats.released == 30 && stats.rejected_full == 20);
  CHECK(stats.discarded == 0 && stats.rejected_input == 0 && stats.occupied == 0);
  CHECK(stats.rejected_stopped == 0 && stats.high_watermark == 3);
  finish(&queue);
}

static void test_failed_input(void)
{
  struct k2b_input_queue *queue = NULL;
  struct k2b_input_view held, next;
  struct k2b_input_stats stats;
  unsigned char bytes[] = { 1, 2, 3, 4, 5 };
  unsigned char expected[] = { 1, 2, 3, 4 };
  LENTRY entry, later;
  DECODE_UNIT unit = make_unit(&entry, bytes, 4, 10);
  CHECK(k2b_input_queue_create(&queue, 3, 4) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &held, 0) == K2B_QUEUE_OK);
  unit.frameNumber = 11;
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  /* Validate a bad later fragment before copying any earlier fragment. */
  memset(&later, 0, sizeof(later));
  entry.length = 2;
  entry.next = &later;
  later.data = (char *)bytes + 2;
  later.length = 2;
  later.bufferType = -1;
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_INVALID);
  CHECK(memcmp(held.data, expected, sizeof(expected)) == 0);
  CHECK(memcmp(bytes, expected, sizeof(expected)) == 0);
  unit = make_unit(&entry, bytes, sizeof(bytes), 12);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_TOO_LARGE);
  CHECK(memcmp(held.data, expected, sizeof(expected)) == 0);
  CHECK(k2b_input_queue_push(queue, NULL) == K2B_QUEUE_INVALID);
  stats = snapshot(queue);
  CHECK(stats.accepted == 2 && stats.rejected_input == 3 && stats.occupied == 2);
  CHECK(stats.queued == 1 && stats.high_watermark == 2 && stats.rejected_full == 0);
  CHECK(k2b_input_queue_release(queue, &held) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &next, 0) == K2B_QUEUE_OK);
  CHECK(next.unit.frame_number == 11 && next.unit.bytes == 4);
  CHECK(memcmp(next.data, expected, sizeof(expected)) == 0);
  CHECK(k2b_input_queue_release(queue, &next) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &next, 0) == K2B_QUEUE_EMPTY);
  unit = make_unit(&entry, bytes, 4, 13);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &next, 0) == K2B_QUEUE_OK);
  CHECK(next.unit.frame_number == 13);
  CHECK(k2b_input_queue_release(queue, &next) == K2B_QUEUE_OK);
  stats = snapshot(queue);
  CHECK(stats.accepted == 3 && stats.released == 3 && stats.discarded == 0);
  finish(&queue);
}

static void test_lease_identity(void)
{
  struct k2b_input_queue *queue = NULL, *other = NULL;
  struct k2b_input_view first, current, foreign, bad;
  struct k2b_input_stats stats;
  unsigned char bytes[] = { 4, 5 };
  LENTRY entry;
  DECODE_UNIT unit = make_unit(&entry, bytes, sizeof(bytes), 1);
  CHECK(k2b_input_queue_create(&queue, 1, 2) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_create(&other, 1, 2) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(other, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &first, 0) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(other, &foreign, 0) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_release(queue, &foreign) == K2B_QUEUE_INVALID);
  bad = first;
  bad.owner = other;
  CHECK(k2b_input_queue_release(queue, &bad) == K2B_QUEUE_INVALID);
  bad = first;
  bad.token++;
  CHECK(k2b_input_queue_release(queue, &bad) == K2B_QUEUE_INVALID);
  bad = first;
  bad.data++;
  CHECK(k2b_input_queue_release(queue, &bad) == K2B_QUEUE_INVALID);
  stats = snapshot(queue);
  CHECK(stats.leased == 1 && stats.occupied == 1 && stats.released == 0);
  CHECK(memcmp(first.data, bytes, sizeof(bytes)) == 0);
  CHECK(k2b_input_queue_release(queue, &first) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_release(queue, &first) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &current, 0) == K2B_QUEUE_OK);
  CHECK(current.data == first.data && current.token > first.token);
  CHECK(k2b_input_queue_release(queue, &first) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_release(other, &current) == K2B_QUEUE_INVALID);
  CHECK(k2b_input_queue_release(queue, &current) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_release(other, &foreign) == K2B_QUEUE_OK);
  stats = snapshot(queue);
  CHECK(stats.accepted == 2 && stats.released == 2 && stats.occupied == 0);
  finish(&queue);
  finish(&other);
}

static void test_discard_stop(void)
{
  struct k2b_input_queue *queue = NULL;
  struct k2b_input_view held, untouched, poison;
  struct k2b_input_stats stats;
  unsigned char bytes[] = { 7, 8, 9 };
  LENTRY entry;
  DECODE_UNIT unit = make_unit(&entry, bytes, sizeof(bytes), 1);
  CHECK(k2b_input_queue_create(&queue, 3, 3) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_discard_pending(queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_discard_pending(queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_discard_pending(queue) == K2B_QUEUE_OK);
  stats = snapshot(queue);
  CHECK(stats.discarded == 2 && stats.occupied == 0);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_take(queue, &held, 0) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_discard_pending(queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_discard_pending(queue) == K2B_QUEUE_OK);
  CHECK(memcmp(held.data, bytes, sizeof(bytes)) == 0);
  stats = snapshot(queue);
  CHECK(stats.discarded == 4 && stats.occupied == 1 && stats.queued == 0 && stats.leased == 1);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(memcmp(held.data, bytes, sizeof(bytes)) == 0);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_BUSY);
  CHECK(k2b_input_queue_stop(queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_stop(queue) == K2B_QUEUE_OK);
  CHECK(k2b_input_queue_discard_pending(queue) == K2B_QUEUE_OK);
  CHECK(memcmp(held.data, bytes, sizeof(bytes)) == 0);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_BUSY);
  /* Stopped wins over both an outstanding lease and capacity/input checks. */
  CHECK(k2b_input_queue_push(queue, NULL) == K2B_QUEUE_STOPPED);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_STOPPED);
  memset(&untouched, 0xa5, sizeof(untouched));
  memcpy(&poison, &untouched, sizeof(poison));
  CHECK(k2b_input_queue_take(queue, &untouched, 0) == K2B_QUEUE_STOPPED);
  CHECK(memcmp(&untouched, &poison, sizeof(poison)) == 0);
  CHECK(k2b_input_queue_take(queue, &untouched, 1) == K2B_QUEUE_STOPPED);
  CHECK(memcmp(&untouched, &poison, sizeof(poison)) == 0);
  stats = snapshot(queue);
  CHECK(stats.accepted == 7 && stats.discarded == 6 && stats.released == 0);
  CHECK(stats.rejected_stopped == 2 && stats.rejected_full == 0 && stats.rejected_input == 0);
  CHECK(stats.high_watermark == 3 && stats.stopped == 1 && stats.occupied == 1);
  CHECK(k2b_input_queue_release(queue, &held) == K2B_QUEUE_OK);
  stats = snapshot(queue);
  CHECK(stats.released == 1 && stats.occupied == 0 && stats.leased == 0);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_OK);
  CHECK(queue == NULL);
}

struct waiter_context {
  struct k2b_input_queue *queue;
  struct k2b_input_view view;
  int result;
};

static void *waiter(void *argument)
{
  struct waiter_context *context = argument;
  context->result = k2b_input_queue_take(context->queue, &context->view, 1);
  return NULL;
}

static void wait_until_waiting(struct k2b_input_queue *queue)
{
  struct timespec delay = { 0, 1000000 };
  int attempt;
  for (attempt = 0; attempt < 5000; attempt++) {
    if (snapshot(queue).waiting == 1)
      return;
    CHECK(nanosleep(&delay, NULL) == 0);
  }
  CHECK(!"consumer did not enter condition wait within five seconds");
}

static void test_waiters(void)
{
  struct k2b_input_queue *queue = NULL;
  struct waiter_context context;
  struct k2b_input_view poison;
  struct k2b_input_stats stats;
  pthread_t thread;
  unsigned char bytes[] = { 3, 2, 1 };
  LENTRY entry;
  DECODE_UNIT unit = make_unit(&entry, bytes, sizeof(bytes), 42);
  CHECK(k2b_input_queue_create(&queue, 2, 3) == K2B_QUEUE_OK);
  memset(&context, 0, sizeof(context));
  context.queue = queue;
  CHECK(pthread_create(&thread, NULL, waiter, &context) == 0);
  wait_until_waiting(queue);
  CHECK(k2b_input_queue_push(queue, &unit) == K2B_QUEUE_OK);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(context.result == K2B_QUEUE_OK);
  CHECK(context.view.unit.frame_number == 42);
  CHECK(memcmp(context.view.data, bytes, sizeof(bytes)) == 0);
  stats = snapshot(queue);
  CHECK(stats.waiting == 0 && stats.leased == 1 && stats.occupied == 1);
  CHECK(k2b_input_queue_release(queue, &context.view) == K2B_QUEUE_OK);
  memset(&context.view, 0xa5, sizeof(context.view));
  memcpy(&poison, &context.view, sizeof(poison));
  CHECK(pthread_create(&thread, NULL, waiter, &context) == 0);
  wait_until_waiting(queue);
  CHECK(k2b_input_queue_stop(queue) == K2B_QUEUE_OK);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(context.result == K2B_QUEUE_STOPPED);
  CHECK(memcmp(&context.view, &poison, sizeof(poison)) == 0);
  stats = snapshot(queue);
  CHECK(stats.waiting == 0 && stats.stopped == 1 && stats.occupied == 0);
  CHECK(stats.accepted == 1 && stats.released == 1 && stats.discarded == 0);
  CHECK(k2b_input_queue_destroy(&queue) == K2B_QUEUE_OK);
}

#define STRESS_FRAMES 12000
#define STRESS_BYTES 64

/* Each thread owns its result fields; the main thread reads them after join. */
struct stress_context {
  struct k2b_input_queue *queue;
  int failed;
  unsigned int completed;
  uint64_t full_retries;
};

static unsigned char payload_byte(int frame, size_t offset)
{
  return (unsigned char)((unsigned int)frame * 37u + (unsigned int)offset * 13u);
}

static void stress_fail(struct stress_context *context)
{
  context->failed = 1;
  (void)k2b_input_queue_stop(context->queue);
}

static void *producer(void *argument)
{
  struct stress_context *context = argument;
  unsigned char bytes[STRESS_BYTES];
  LENTRY entry;
  DECODE_UNIT unit;
  int frame;
  for (frame = 0; frame < STRESS_FRAMES; frame++) {
    size_t index;
    int result;
    for (index = 0; index < sizeof(bytes); index++)
      bytes[index] = payload_byte(frame, index);
    unit = make_unit(&entry, bytes, sizeof(bytes), frame);
    do {
      result = k2b_input_queue_push(context->queue, &unit);
      if (result == K2B_QUEUE_FULL) {
        context->full_retries++;
        sched_yield();
      }
    } while (result == K2B_QUEUE_FULL); /* Retry is a test driver policy only. */
    if (result != K2B_QUEUE_OK) {
      stress_fail(context);
      return NULL;
    }
    memset(bytes, 0xdd, sizeof(bytes));
    context->completed++;
  }
  return NULL;
}

static int valid_payload(const struct k2b_input_view *view, int frame)
{
  size_t index;
  if (view->unit.frame_number != frame || view->unit.bytes != STRESS_BYTES ||
      view->unit.pts_us != (int64_t)frame * 1000 ||
      view->unit.receive_us != (uint64_t)frame * 1000 + 1 ||
      view->unit.enqueue_us != (uint64_t)frame * 1000 + 2)
    return 0;
  for (index = 0; index < STRESS_BYTES; index++) {
    if (view->data[index] != payload_byte(frame, index))
      return 0;
  }
  return 1;
}

static void *consumer(void *argument)
{
  struct stress_context *context = argument;
  struct k2b_input_view view;
  uint64_t last_token = 0;
  int frame;
  for (frame = 0; frame < STRESS_FRAMES; frame++) {
    if (k2b_input_queue_take(context->queue, &view, 1) != K2B_QUEUE_OK) {
      stress_fail(context);
      return NULL;
    }
    if (view.owner != context->queue || view.token <= last_token ||
        !valid_payload(&view, frame)) {
      (void)k2b_input_queue_release(context->queue, &view);
      stress_fail(context);
      return NULL;
    }
    last_token = view.token;
    sched_yield();
    if (!valid_payload(&view, frame)) {
      (void)k2b_input_queue_release(context->queue, &view);
      stress_fail(context);
      return NULL;
    }
    if (k2b_input_queue_release(context->queue, &view) != K2B_QUEUE_OK) {
      stress_fail(context);
      return NULL;
    }
    context->completed++;
  }
  return NULL;
}

static void test_stress(void)
{
  struct k2b_input_queue *queue = NULL;
  struct stress_context writer = { 0 }, reader = { 0 };
  struct k2b_input_stats stats;
  pthread_t writer_thread, reader_thread;
  struct timespec delay = { 0, 1000000 };
  CHECK(k2b_input_queue_create(&queue, K2B_INPUT_QUEUE_MAX_SLOTS, STRESS_BYTES) == K2B_QUEUE_OK);
  writer.queue = reader.queue = queue;
  CHECK(pthread_create(&reader_thread, NULL, consumer, &reader) == 0);
  CHECK(pthread_create(&writer_thread, NULL, producer, &writer) == 0);
  /* Exercise locked snapshots concurrently with both worker threads. */
  do {
    stats = snapshot(queue);
    CHECK(stats.accepted == stats.released + stats.discarded + stats.occupied);
    CHECK(stats.waiting <= 1);
    if (stats.stopped)
      break;
    CHECK(nanosleep(&delay, NULL) == 0);
  } while (stats.released < STRESS_FRAMES);
  CHECK(pthread_join(writer_thread, NULL) == 0);
  CHECK(pthread_join(reader_thread, NULL) == 0);
  CHECK(writer.failed == 0 && reader.failed == 0);
  CHECK(writer.completed == STRESS_FRAMES && reader.completed == STRESS_FRAMES);
  stats = snapshot(queue);
  CHECK(stats.accepted == STRESS_FRAMES && stats.released == STRESS_FRAMES);
  CHECK(stats.rejected_full == writer.full_retries && stats.rejected_input == 0);
  CHECK(stats.rejected_stopped == 0 && stats.discarded == 0);
  CHECK(stats.occupied == 0 && stats.queued == 0 && stats.waiting == 0 && stats.leased == 0);
  CHECK(stats.high_watermark > 0 && stats.high_watermark <= K2B_INPUT_QUEUE_MAX_SLOTS);
  finish(&queue);
}

int main(void)
{
  /* Process-wide bound also catches deadlocks in join or queue mutexes. */
  alarm(30);
  test_lifecycle();
  test_parameters();
  test_fifo();
  test_failed_input();
  test_lease_identity();
  test_discard_stop();
  test_waiters();
  test_stress();
  alarm(0);
  puts("input queue tests passed");
  return 0;
}
