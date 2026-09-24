#include "access_unit.h"

#include <string.h>

int k2b_access_unit_copy(struct k2b_access_unit *out, void *storage,
                         size_t capacity, const DECODE_UNIT *unit)
{
  struct k2b_access_unit result = { 0 };
  const LENTRY *entry;
  size_t expected;
  size_t total = 0;
  unsigned char *destination = storage;

  if (out == NULL || storage == NULL || unit == NULL ||
      unit->fullLength <= 0 || (size_t)unit->fullLength > K2B_AU_MAX_BYTES ||
      unit->bufferList == NULL || unit->presentationTimeUs > INT64_MAX ||
      unit->hdrActive ||
      (unit->frameType != FRAME_TYPE_PFRAME && unit->frameType != FRAME_TYPE_IDR) ||
      (unit->colorspace != COLORSPACE_REC_601 && unit->colorspace != COLORSPACE_REC_709))
    return K2B_AU_INVALID;

  expected = (size_t)unit->fullLength;
  for (entry = unit->bufferList; entry != NULL; entry = entry->next) {
    if (entry->data == NULL || entry->length <= 0 ||
        (size_t)entry->length > expected - total ||
        (entry->bufferType != BUFFER_TYPE_PICDATA &&
         entry->bufferType != BUFFER_TYPE_SPS && entry->bufferType != BUFFER_TYPE_PPS))
      return K2B_AU_INVALID;
    /* Positive lengths bounded by the declared total also terminate cycles. */
    total += (size_t)entry->length;
  }
  if (total != expected)
    return K2B_AU_INVALID;
  if (capacity < expected)
    return K2B_AU_NO_SPACE;

  for (entry = unit->bufferList; entry != NULL; entry = entry->next) {
    memcpy(destination, entry->data, (size_t)entry->length);
    destination += (size_t)entry->length;
  }
  result.bytes = expected;
  result.pts_us = (int64_t)unit->presentationTimeUs;
  result.receive_us = unit->receiveTimeUs;
  result.enqueue_us = unit->enqueueTimeUs;
  result.frame_number = unit->frameNumber;
  result.frame_type = unit->frameType;
  result.colorspace = unit->colorspace;
  *out = result;
  return K2B_AU_OK;
}
