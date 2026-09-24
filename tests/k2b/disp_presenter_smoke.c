#include "disp_presenter.h"
#include <stdio.h>

/* Native manual test only: opens the real display and submits blank commits.
 * Closing the last /dev/disp fd may switch off HDMI on this vendor kernel. */
int main(void)
{
  struct k2b_disp *disp = NULL;
  if (k2b_disp_open(&disp) < 0) {
    perror("display open");
    return 1;
  }
  if (k2b_disp_retire(disp) < 0) {
    perror("display blank/fence drain: recovery required");
    return 2;
  }
  k2b_disp_close(disp);
  puts("PASS: native display open, blank commits and release-fence drain");
  return 0;
}
