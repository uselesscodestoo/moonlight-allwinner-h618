/*
 * Display-path benchmark for the H618 zero-copy work.
 *
 * Measures how fast this exact EGL/GLES presentation path can go at a given
 * window size, independently of any stream source, so "can this board present
 * 1080p60?" is answered by a controlled number instead of by end-to-end fps
 * (which depends on what the streaming host sends).
 *
 * Modes:
 *   egl_bench [width] [height] [frames] [software|nv12]
 *     software - egl_draw() with synthetic YUV420P planes (3 texture uploads,
 *                i.e. the CPU-download path's presentation half)
 *     nv12     - egl_draw_dmabuf_nv12() with a synthetic NV12 dma-buf allocated
 *                from /dev/dma_heap/system (the full zero-copy path, including
 *                importing and sampling the buffer)
 *
 * Build (from the repo root):
 *   gcc -O2 -o /tmp/egl_bench tools/egl_bench.c src/video/egl.c \
 *       $(pkg-config --cflags --libs egl glesv2 x11) -I src
 *
 * Notes: the window is created exactly like the streaming path does
 * (full-screen at the given size, DISPLAY from the environment) and the
 * reported rate includes eglSwapBuffers, so the number is the real present
 * ceiling.  Keep the monitor out of DPMS (xset s off -dpms) or every swap
 * costs ~750 ms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "video/egl.h"

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* Allocate a dma-buf from the system heap and map it; returns the fd. */
static int alloc_dmabuf(size_t size, void** map) {
  struct dma_heap_allocation_data data = {
    .len = size,
    .fd_flags = O_RDWR | O_CLOEXEC,
    .heap_flags = 0,
  };
  int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
  if (heap < 0) {
    perror("open /dev/dma_heap/system");
    return -1;
  }
  if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
    perror("DMA_HEAP_IOCTL_ALLOC");
    close(heap);
    return -1;
  }
  close(heap);

  *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
  if (*map == MAP_FAILED) {
    perror("mmap dma-buf");
    close(data.fd);
    return -1;
  }
  return data.fd;
}

int main(int argc, char** argv) {
  int width = argc > 1 ? atoi(argv[1]) : 1920;
  int height = argc > 2 ? atoi(argv[2]) : 1080;
  int frames = argc > 3 ? atoi(argv[3]) : 400;
  bool use_nv12 = argc > 4 && strcmp(argv[4], "nv12") == 0;
  int y_pitch = width;
  int uv_offset = y_pitch * ((height + 31) / 32 * 32);
  size_t buf_size = uv_offset + y_pitch * ((height / 2 + 31) / 32 * 32);

  Display* dpy = XOpenDisplay(NULL);
  if (dpy == NULL) {
    fprintf(stderr, "cannot open display\n");
    return 1;
  }

  Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, width, height, 0, 0, 0);
  XStoreName(dpy, win, "egl_bench");
  XMapWindow(dpy, win);

  /* Ask the window manager to make this fullscreen: xfwm4 (and other
   * compositors) unredirect fullscreen windows, which is what the streaming
   * client's window is, and the difference is large (3-10x) because an
   * overlapping window has to go through the compositor every frame. */
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

  if (use_nv12) {
    void* map = NULL;
    int fd = alloc_dmabuf(buf_size, &map);
    if (fd < 0)
      return 1;

    /* Synthetic limited-range NV12: a moving gradient so nothing can be
     * skipped as "unchanged". */
    unsigned char* p = map;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
        p[y * y_pitch + x] = (unsigned char) (16 + ((x + y) & 0xff) * 219 / 255);
    for (int y = 0; y < height / 2; y++)
      for (int x = 0; x < width; x++)
        p[uv_offset + y * y_pitch + x] = 128;

    double t0 = now_ms();
    for (int i = 0; i < frames + 20; i++) {
      /* move the gradient a little each frame so the content really changes */
      for (int x = 0; x < width; x++)
        p[x] = (unsigned char) (16 + ((x + i * 3) & 0xff) * 219 / 255);
      double s = now_ms();
      if (egl_draw_dmabuf_nv12(fd, buf_size, width, height, y_pitch, uv_offset) != 0) {
        fprintf(stderr, "egl_draw_dmabuf_nv12 failed\n");
        return 1;
      }
      if (i == 19)
        t0 = now_ms();          /* skip the first frames (warm up) */
    }
    double dt = now_ms() - t0;
    printf("nv12 zero-copy %dx%d: %d frames in %.0f ms = %.1f fps (%.2f ms/frame)\n",
           width, height, frames, dt, 1000.0 * frames / dt, dt / frames);
  } else {
    uint8_t* planes[3];
    planes[0] = malloc(width * height);
    planes[1] = malloc(width / 2 * height / 2);
    planes[2] = malloc(width / 2 * height / 2);
    for (int i = 0; i < 3; i++)
      memset(planes[i], i == 0 ? 128 : 128, i == 0 ? width * height : width / 2 * height / 2);

    double t0 = 0;
    for (int i = 0; i < frames + 20; i++) {
      memset(planes[0], 16 + (i & 0x7f), width * height);
      if (i == 19)
        t0 = now_ms();
      egl_draw(planes);
    }
    double dt = now_ms() - t0;
    printf("software upload %dx%d: %d frames in %.0f ms = %.1f fps (%.2f ms/frame)\n",
           width, height, frames, dt, 1000.0 * frames / dt, dt / frames);
  }

  egl_destroy();
  return 0;
}
