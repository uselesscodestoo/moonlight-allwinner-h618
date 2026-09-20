/* Standalone check of the production CMA NV12 external-texture renderer. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <X11/Xlib.h>

#include "video/egl.h"

int main(void) {
  Display* dpy = XOpenDisplay(NULL);
  if (dpy == NULL) {
    fprintf(stderr, "cannot open display\n");
    return 1;
  }

  Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                  0, 0, 64, 64, 0, 0, 0);
  XMapWindow(dpy, win);
  XFlush(dpy);

  setenv("MOONLIGHT_ZC_EXTERNAL", "1", 1);
  egl_init(dpy, (NativeWindowType) win, 64, 64);
  int rc = egl_nv12_external_selftest();

  egl_destroy();
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return rc;
}
