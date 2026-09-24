#include "access_unit.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

#define CHECK(name, expression) do { \
  checks++; \
  if (!(expression)) { \
    fprintf(stderr, "FAIL: %s: %s (line %d)\n", (name), #expression, __LINE__); \
    failures++; \
  } \
} while (0)

static DECODE_UNIT valid_unit(LENTRY *entry)
{
  DECODE_UNIT unit = { 0 };
  unit.fullLength = entry->length;
  unit.bufferList = entry;
  unit.frameType = FRAME_TYPE_PFRAME;
  unit.colorspace = COLORSPACE_REC_709;
  unit.frameNumber = 123;
  unit.presentationTimeUs = 456;
  unit.receiveTimeUs = 789;
  unit.enqueueTimeUs = 901;
  return unit;
}

static int all_bytes(const unsigned char *bytes, size_t length,
                     unsigned char expected)
{
  size_t i;
  for (i = 0; i < length; i++) {
    if (bytes[i] != expected)
      return 0;
  }
  return 1;
}

static void check_success(const char *name, const DECODE_UNIT *unit,
                          const void *expected, size_t capacity)
{
  struct k2b_access_unit out;
  unsigned char unit_before[sizeof(*unit)];
  unsigned char *allocation = malloc(capacity + 32);
  int result;

  CHECK(name, allocation != NULL);
  if (allocation == NULL)
    return;
  memset(allocation, 0xa5, capacity + 32);
  memset(&out, 0x5a, sizeof(out));
  memcpy(unit_before, unit, sizeof(*unit));
  result = k2b_access_unit_copy(&out, allocation + 16, capacity, unit);
  CHECK(name, result == K2B_AU_OK);
  CHECK(name, memcmp(unit_before, unit, sizeof(*unit)) == 0);
  if (result == K2B_AU_OK) {
    CHECK(name, out.bytes == (size_t)unit->fullLength);
    CHECK(name, out.pts_us == (int64_t)unit->presentationTimeUs);
    CHECK(name, out.receive_us == unit->receiveTimeUs);
    CHECK(name, out.enqueue_us == unit->enqueueTimeUs);
    CHECK(name, out.frame_number == unit->frameNumber);
    CHECK(name, out.frame_type == unit->frameType);
    CHECK(name, out.colorspace == unit->colorspace);
    CHECK(name, memcmp(allocation + 16, expected, (size_t)unit->fullLength) == 0);
    CHECK(name, all_bytes(allocation, 16, 0xa5));
    CHECK(name, all_bytes(allocation + 16 + unit->fullLength,
                          capacity - (size_t)unit->fullLength + 16, 0xa5));
  }
  free(allocation);
}

static void test_valid_units(void)
{
  char picture[] = { 0, 0, 0, 1, 0x41, 0, 0x32, 0 };
  char picture_before[sizeof(picture)];
  LENTRY entry = { NULL, picture, sizeof(picture), BUFFER_TYPE_PICDATA };
  unsigned char entry_before[sizeof(entry)];
  DECODE_UNIT unit = valid_unit(&entry);

  memcpy(picture_before, picture, sizeof(picture));
  memcpy(entry_before, &entry, sizeof(entry));
  check_success("single P frame, exact capacity", &unit, picture_before,
                sizeof(picture));
  check_success("single P frame, unused tail", &unit, picture_before, 32);
  CHECK("source node unchanged", memcmp(entry_before, &entry, sizeof(entry)) == 0);
  CHECK("source bytes unchanged", memcmp(picture_before, picture, sizeof(picture)) == 0);

  unit.presentationTimeUs = 0;
  unit.receiveTimeUs = 0;
  unit.enqueueTimeUs = 0;
  unit.frameNumber = 0;
  unit.colorspace = COLORSPACE_REC_601;
  check_success("zero metadata and Rec.601", &unit, picture_before, 32);

  unit.presentationTimeUs = INT64_MAX;
  unit.receiveTimeUs = UINT64_MAX;
  unit.enqueueTimeUs = UINT64_MAX;
  unit.frameNumber = INT_MAX;
  check_success("maximum metadata", &unit, picture_before, 32);
  unit.frameNumber = INT_MIN;
  check_success("wrapped negative frame number", &unit, picture_before, 32);
  unit.frameNumber = -1;
  unit.receiveTimeUs = UINT64_MAX;
  unit.enqueueTimeUs = 0;
  check_success("frame number and timestamps are copied without ordering", &unit,
                picture_before, 32);
}

static void test_fragmented_idr(void)
{
  char sps[] = { 0, 0, 0, 1, 0x67, 0 };
  char pps[] = { 0, 0, 1, 0x68, 0 };
  char idr[] = { 0, 0, 1, 0x65, 0, 3, 0, 0 };
  const char expected[] = { 0, 0, 0, 1, 0x67, 0, 0, 0, 1, 0x68, 0,
                           0, 0, 1, 0x65, 0, 3, 0, 0 };
  LENTRY entries[] = {
    { &entries[1], sps, sizeof(sps), BUFFER_TYPE_SPS },
    { &entries[2], pps, sizeof(pps), BUFFER_TYPE_PPS },
    { NULL, idr, sizeof(idr), BUFFER_TYPE_PICDATA }
  };
  unsigned char entries_before[sizeof(entries)];
  DECODE_UNIT unit = valid_unit(entries);
  struct k2b_access_unit out;
  unsigned char storage[sizeof(expected) + 16];
  int result;

  unit.fullLength = sizeof(expected);
  unit.frameType = FRAME_TYPE_IDR;
  memcpy(entries_before, entries, sizeof(entries));
  check_success("SPS/PPS/IDR fragments preserve zeros and start codes", &unit,
                expected, sizeof(storage));
  CHECK("all source nodes unchanged",
        memcmp(entries_before, entries, sizeof(entries)) == 0);
  CHECK("SPS bytes unchanged", memcmp(sps, expected, sizeof(sps)) == 0);
  CHECK("PPS bytes unchanged", memcmp(pps, expected + sizeof(sps), sizeof(pps)) == 0);
  CHECK("IDR bytes unchanged",
        memcmp(idr, expected + sizeof(sps) + sizeof(pps), sizeof(idr)) == 0);

  memset(storage, 0xa5, sizeof(storage));
  result = k2b_access_unit_copy(&out, storage, sizeof(storage), &unit);
  CHECK("independent copy succeeds", result == K2B_AU_OK);
  memset(sps, 0xff, sizeof(sps));
  memset(pps, 0xff, sizeof(pps));
  memset(idr, 0xff, sizeof(idr));
  memset(entries, 0, sizeof(entries));
  memset(&unit, 0, sizeof(unit));
  if (result == K2B_AU_OK) {
    CHECK("copy survives source changes", memcmp(storage, expected, sizeof(expected)) == 0);
    CHECK("independent copy tail intact", all_bytes(storage + sizeof(expected), 16, 0xa5));
    CHECK("copied metadata survives source changes", out.bytes == sizeof(expected) &&
          out.frame_number == 123 && out.frame_type == FRAME_TYPE_IDR && out.pts_us == 456);
  }
}

static void test_hevc_idr(void)
{
  char bytes[] = {0,0,1,0x40,1, 0,0,1,0x42,1,
                  0,0,1,0x44,1, 0,0,1,0x26,1};
  LENTRY entries[] = {
    {&entries[1], bytes, 5, BUFFER_TYPE_VPS},
    {&entries[2], bytes + 5, 5, BUFFER_TYPE_SPS},
    {&entries[3], bytes + 10, 5, BUFFER_TYPE_PPS},
    {NULL, bytes + 15, 5, BUFFER_TYPE_PICDATA}
  };
  DECODE_UNIT unit = valid_unit(entries);
  unit.fullLength = sizeof(bytes);
  unit.frameType = FRAME_TYPE_IDR;
  check_success("HEVC VPS/SPS/PPS/IDR byte order", &unit, bytes, 32);
}

static void test_maximum_unit(void)
{
  char *picture = malloc(K2B_AU_MAX_BYTES);
  LENTRY entry = { NULL, picture, K2B_AU_MAX_BYTES, BUFFER_TYPE_PICDATA };
  DECODE_UNIT unit = valid_unit(&entry);
  size_t i;

  CHECK("maximum source allocation", picture != NULL);
  if (picture == NULL)
    return;
  for (i = 0; i < K2B_AU_MAX_BYTES; i++)
    picture[i] = (char)(i % 251);
  check_success("4 MiB maximum succeeds", &unit, picture, K2B_AU_MAX_BYTES);
  free(picture);
}

static void check_failure(const char *name, const DECODE_UNIT *unit,
                          size_t capacity, int expected, int null_out,
                          int null_storage)
{
  struct k2b_access_unit out;
  unsigned char out_before[sizeof(out)];
  unsigned char storage[64];
  unsigned char storage_before[sizeof(storage)];
  unsigned char unit_before[sizeof(DECODE_UNIT)];
  int result;

  memset(&out, 0x5a, sizeof(out));
  memcpy(out_before, &out, sizeof(out));
  memset(storage, 0xa5, sizeof(storage));
  memcpy(storage_before, storage, sizeof(storage));
  if (unit != NULL)
    memcpy(unit_before, unit, sizeof(*unit));
  result = k2b_access_unit_copy(null_out ? NULL : &out,
                                null_storage ? NULL : storage + 16,
                                capacity, unit);
  CHECK(name, result == expected);
  CHECK(name, memcmp(out_before, &out, sizeof(out)) == 0);
  CHECK(name, memcmp(storage_before, storage, sizeof(storage)) == 0);
  if (unit != NULL)
    CHECK(name, memcmp(unit_before, unit, sizeof(*unit)) == 0);
}

#define CHECK_BAD_UNIT(name, field, value) do { \
  DECODE_UNIT changed = valid_unit(&entry); \
  changed.field = (value); \
  check_failure((name), &changed, 32, K2B_AU_INVALID, 0, 0); \
} while (0)

#define CHECK_BAD_ENTRY(name, field, value) do { \
  LENTRY changed = entry; \
  DECODE_UNIT changed_unit = valid_unit(&changed); \
  changed.field = (value); \
  check_failure((name), &changed_unit, 32, K2B_AU_INVALID, 0, 0); \
} while (0)

static void test_invalid_units(void)
{
  char picture[] = { 0, 0, 1, 0x41, 0 };
  char picture_before[sizeof(picture)];
  LENTRY entry = { NULL, picture, sizeof(picture), BUFFER_TYPE_PICDATA };
  unsigned char entry_before[sizeof(entry)];
  DECODE_UNIT unit = valid_unit(&entry);

  memcpy(picture_before, picture, sizeof(picture));
  memcpy(entry_before, &entry, sizeof(entry));
  check_failure("null unit", NULL, 32, K2B_AU_INVALID, 0, 0);
  check_failure("null output", &unit, 32, K2B_AU_INVALID, 1, 0);
  check_failure("null storage", &unit, 32, K2B_AU_INVALID, 0, 1);
  check_failure("all null", NULL, 0, K2B_AU_INVALID, 1, 1);
  CHECK_BAD_UNIT("zero full length", fullLength, 0);
  CHECK_BAD_UNIT("negative full length", fullLength, -1);
  CHECK_BAD_UNIT("minimum full length", fullLength, INT_MIN);
  CHECK_BAD_UNIT("above 4 MiB cap", fullLength, K2B_AU_MAX_BYTES + 1);
  CHECK_BAD_UNIT("INT_MAX full length", fullLength, INT_MAX);
  CHECK_BAD_UNIT("empty list", bufferList, NULL);
  CHECK_BAD_UNIT("undersum", fullLength, sizeof(picture) + 1);
  CHECK_BAD_UNIT("oversum", fullLength, sizeof(picture) - 1);
  CHECK_BAD_UNIT("PTS above signed maximum", presentationTimeUs, (uint64_t)INT64_MAX + 1);
  CHECK_BAD_UNIT("PTS unsigned maximum", presentationTimeUs, UINT64_MAX);
  CHECK_BAD_UNIT("HDR", hdrActive, true);
  CHECK_BAD_UNIT("Rec.2020", colorspace, COLORSPACE_REC_2020);
  CHECK_BAD_UNIT("unknown colorspace", colorspace, UINT8_MAX);
  CHECK_BAD_UNIT("unknown frame type", frameType, 2);
  CHECK_BAD_UNIT("negative frame type", frameType, -1);
  CHECK_BAD_ENTRY("null fragment data", data, NULL);
  CHECK_BAD_ENTRY("zero fragment length", length, 0);
  CHECK_BAD_ENTRY("negative fragment length", length, -1);
  CHECK_BAD_ENTRY("minimum fragment length", length, INT_MIN);
  CHECK_BAD_ENTRY("INT_MAX fragment length", length, INT_MAX);
  CHECK_BAD_ENTRY("unknown buffer type", bufferType, 4);
  CHECK_BAD_ENTRY("negative buffer type", bufferType, -1);
  CHECK_BAD_ENTRY("self cycle", next, &changed);
  check_failure("zero capacity", &unit, 0, K2B_AU_NO_SPACE, 0, 0);
  check_failure("capacity one byte short", &unit, sizeof(picture) - 1,
                K2B_AU_NO_SPACE, 0, 0);
  CHECK("failure source node unchanged", memcmp(entry_before, &entry, sizeof(entry)) == 0);
  CHECK("failure source data unchanged", memcmp(picture_before, picture, sizeof(picture)) == 0);
}

static void test_bad_later_fragments(void)
{
  char first[] = { 0, 0, 1, 0x67 };
  char last[] = { 0, 0, 1, 0x65 };
  LENTRY entries[] = {
    { &entries[1], first, sizeof(first), BUFFER_TYPE_SPS },
    { NULL, last, sizeof(last), BUFFER_TYPE_PICDATA }
  };
  DECODE_UNIT unit = valid_unit(entries);
  unsigned char entries_before[sizeof(entries)];

  unit.fullLength = sizeof(first) + sizeof(last);
  unit.frameType = FRAME_TYPE_IDR;
  entries[1].data = NULL;
  memcpy(entries_before, entries, sizeof(entries));
  check_failure("bad later data, no early copy", &unit, 32, K2B_AU_INVALID, 0, 0);
  check_failure("validation precedes zero capacity", &unit, 0, K2B_AU_INVALID, 0, 0);
  CHECK("bad later nodes unchanged", memcmp(entries_before, entries, sizeof(entries)) == 0);
  entries[1].data = last;
  entries[1].length = 0;
  check_failure("zero later length", &unit, 32, K2B_AU_INVALID, 0, 0);
  entries[1].length = -1;
  check_failure("negative later length", &unit, 32, K2B_AU_INVALID, 0, 0);
  entries[1].length = INT_MAX;
  check_failure("overflowing later length", &unit, 32, K2B_AU_INVALID, 0, 0);
  entries[1].length = sizeof(last);
  entries[1].bufferType = 4;
  check_failure("later unknown type", &unit, 32, K2B_AU_INVALID, 0, 0);
  entries[1].bufferType = BUFFER_TYPE_PICDATA;
  entries[1].next = entries;
  check_failure("two-node cycle", &unit, 32, K2B_AU_INVALID, 0, 0);
  entries[1].next = NULL;
  unit.fullLength++;
  check_failure("later undersum", &unit, 32, K2B_AU_INVALID, 0, 0);
  unit.fullLength -= 2;
  check_failure("later oversum", &unit, 32, K2B_AU_INVALID, 0, 0);
}

int main(void)
{
  test_valid_units();
  test_fragmented_idr();
  test_hevc_idr();
  test_maximum_unit();
  test_invalid_units();
  test_bad_later_fragments();

  if (failures != 0) {
    fprintf(stderr, "%u of %u access-unit checks failed\n", failures, checks);
    return EXIT_FAILURE;
  }
  printf("PASS: %u access-unit checks\n", checks);
  return EXIT_SUCCESS;
}
