#ifndef K2B_DISP_CONFIG_H
#define K2B_DISP_CONFIG_H

#include "frame.h"
#include "disp_uapi.h"

/* Prepare metadata for linear 8-bit NV12 at 1920x1080. Returns 0 on success,
 * -1 for invalid input, leaving out unchanged on failure. The fd is borrowed;
 * this function performs no ioctl, fd inspection, ownership transfer, or pixel
 * reads. frame_id is only a debug label, not proof of presentation.
 * out and frame must not overlap. */
int k2b_disp_config_prepare(struct disp_layer_config2 *out,
                            const struct k2b_frame *frame, uint32_t frame_id);

#endif
