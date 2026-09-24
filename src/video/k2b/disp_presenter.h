#ifndef K2B_DISP_PRESENTER_H
#define K2B_DISP_PRESENTER_H

#include "frame.h"

struct k2b_disp;

/* Single worker owns these calls. Uses the vendor compositor's release fence;
 * live testing, not the fence alone, establishes the display result. */
int k2b_disp_open(struct k2b_disp **out);
/* On success caller owns release_fd and must retain its VideoPicture until
 * that fence signals. On failure retain the picture until retire succeeds. */
int k2b_disp_present(struct k2b_disp *disp, const struct k2b_frame *frame,
                     uint32_t frame_id, int *release_fd);
/* Disable/reap the layer using blank commits and bounded fence waits. On
 * failure do not free scanout buffers or blindly retry device cleanup. */
int k2b_disp_retire(struct k2b_disp *disp);
/* Call only after successful retirement, or before the first submission. */
void k2b_disp_close(struct k2b_disp *disp);

#endif
