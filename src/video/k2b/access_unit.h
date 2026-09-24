#ifndef K2B_ACCESS_UNIT_H
#define K2B_ACCESS_UNIT_H

#include <Limelight.h>
#include <stddef.h>
#include <stdint.h>

#define K2B_AU_MAX_BYTES (4u * 1024u * 1024u)

enum k2b_au_result {
  K2B_AU_OK = 0,
  K2B_AU_INVALID = -1,
  K2B_AU_NO_SPACE = -2
};

struct k2b_access_unit {
  size_t bytes;
  int64_t pts_us;
  uint64_t receive_us;
  uint64_t enqueue_us;
  int frame_number;
  int frame_type;
  uint8_t colorspace;
};

/* Copy an H.264/SDR compressed access unit unchanged into caller storage.
 * All provided non-NULL pointers must be valid for their declared extents;
 * out, storage, unit, source nodes and source data must not overlap. The source
 * unit, chain and data must remain stable throughout this call. No pointers are
 * retained, and there is no allocation, ownership transfer, I/O, pixel access
 * or NAL parsing. Full validation precedes the capacity check and any writes.
 * On any failure, both out and storage remain unchanged. */
int k2b_access_unit_copy(struct k2b_access_unit *out, void *storage,
                         size_t capacity, const DECODE_UNIT *unit);

#endif
