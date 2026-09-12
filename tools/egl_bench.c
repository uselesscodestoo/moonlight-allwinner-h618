/*
 * Display-path benchmark for the H618 zero-copy work.
 *
 * Renders synthetic YUV420P planes through the same egl_draw() the
 * CPU-download path uses (three texture uploads + draw + eglSwapBuffers) and
 * reports the sustainable rate.  That number is dominated by the X server's
 * per-frame work for a client covering the window area, which is what limits
 * any fullscreen streaming client on this board, so it answers "how fast can
 * this display path present <size>?" without depending on what a streaming
 * host happens to send.
 *
 * The zero-copy path is measured inside the application itself (the per-frame
 * c2 value in the "x11:" lines) rather than here: importing a dma-buf that was
 * allocated outside the GPU driver from a test harness is easy to get wrong -
 * a /dev/dma_heap/system buffer made panfrost fault (JOB_BUS_FAULT) and a GBM
 * buffer crashed libgbm on sub-rectangle map/unmap (both on 2026-09-12).
 *
 * Usage:  egl_bench [width] [height] [frames]
 * Build (from the repo root):
 *   gcc -O2 -o /tmp/egl_bench tools/egl_bench.c src/video/egl.c \
 *       $(pkg-config --cflags --libs egl glesv2 x11) -I src \
 *       -I third_party/moonlight-common-c/src
 *
 * The window is created exactly like the streaming client's (same size, same
 * EWMH fullscreen request) and the reported rate includes eglSwapBuffers.
 * Keep the monitor out of DPMS (xset s off -dpms; xset dpms force on): a
 * blanked CRTC costs ~750 ms per swap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "video/egl.h"

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(int argc, char** argv) {
  int width = argc > 1 ? atoi(argv[1]) : 1920;
  int height = argc > 2 ? atoi(argv[2]) : 1080;
  int frames = argc > 3 ? atoi(argv[3]) : 200;

  Display* dpy = XOpenDisplay(NULL);
  if (dpy == NULL) {
    fprintf(stderr, "cannot open display\n");
    return 1;
  }

  Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, width, height, 0, 0, 0);
  XStoreName(dpy, win, "egl_bench");
  XMapWindow(dpy, win);

  /* Ask the window manager for fullscreen: the streaming client is fullscreen,
   * and whether a compositor has to composite the window changes the cost by
   * 3-10x on this SoC. */
  {
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = win;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 1;
    xev.xclient.data.l[1] = fullscreen;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
  }
  XFlush(dpy);
  usleep(500000);

  egl_init(dpy, (NativeWindowType) win, width, height);

  uint8_t* planes[3];
  size_t sizes[3] = { (size_t) width * height,
                      (size_t) (width / 2) * (height / 2),
                      (size_t) (width / 2) * (height / 2) };
  for (int i = 0; i < 3; i++) {
    planes[i] = malloc(sizes[i]);
    memset(planes[i], 128, sizes[i]);
  }

  double t0 = 0;
  for (int i = 0; i < frames + 20; i++) {
    memset(planes[0], 16 + (i & 0x7f), sizes[0]);   /* content really changes */
    if (i == 19)
      t0 = now_ms();
    egl_draw(planes);
  }
  double dt = now_ms() - t0;

  printf("software upload %dx%d: %d frames in %.0f ms = %.1f fps (%.2f ms/frame)\n",
         width, height, frames, dt, 1000.0 * frames / dt, dt / frames);

  egl_destroy();
  return 0;
}
