#ifndef K2B_CEDAR_PICTURE_H
#define K2B_CEDAR_PICTURE_H

#include "frame.h"
#include "cedar_memory.h"
#include <vdecoder.h>

/* Metadata only, with no pixel access or ownership transfer. The caller must
 * retain the picture and synchronize two valid memory_describe views for the
 * entire call. Success does not prove display retirement or permit ReturnPicture.
 * Colors come from the negotiated arguments, and the fd from the memory views.
 * Returns 0 on success, or -1 with EINVAL and an unchanged output on failure. */
int k2b_cedar_picture_frame(const VideoPicture *picture,
                           const struct k2b_cedar_buffer *y,
                           const struct k2b_cedar_buffer *uv,
                           enum k2b_matrix matrix, enum k2b_range range,
                           struct k2b_frame *out);

#endif
