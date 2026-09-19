# H618 zero-copy: localize the per-plane cost

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development. Steps use checkbox syntax.

**Goal:** Decide where the zero-copy display cost lives — image import/cache churn, a per-frame GPU stall (`glFinish`), the draw itself, or `eglSwapBuffers` — using non-invasive instrumentation on the real path.

**Why:** Plan A (two-plane) only reached `c2≈25 ms`. Per-plane fetch ~5 ms; the 0→1 fetch jump (~16 ms) suggests a fixed per-frame stall (fence/sync) plus per-fetch cost. Both caches are already keyed (`ffmpeg_vaapi.c:102`, `egl.c dmabuf_get_nv12`), so this measures rather than guesses.

## Context

- Worktree `F:\temp\wt-moonlight-drm`, branch `h618-drm-kms`, HEAD `23ce36f`.
- Board scratch `~/Downloads/probe/moonlight-dev`; harness `~/Downloads/probe/h618-accept.sh`.
- Baseline (commit `30a42bf`): `default avg_c2≈25 ms`, `trivial≈15 ms/60 fps`.
- Never edit `/home/scy/Downloads/moonlight-embedded`.

---

## Task D1: instrument the path (keep, behind an env var)

**Files:** Modify `src/video/egl.c`.

- [ ] **Step 1: add a clock helper + counters**

If `egl.c` has no millisecond clock, add `#include <time.h>` and near the NV12 state:

```c
static int zc_breakdown = -1;
static unsigned zc_bd_frames, zc_img_hits, zc_img_creates;
static double zc_bd_img, zc_bd_render, zc_bd_swap;

static double zc_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}
```

- [ ] **Step 2: count cache hits/creates in `dmabuf_get_nv12`**

On the cache-hit `return 0`/`return -1` path increment `zc_img_hits`; right before the `dmabuf_nv12_count++` (successful creation) increment `zc_img_creates`. Also increment `zc_img_creates` when a failed creation stores a negative entry.

- [ ] **Step 3: time the three phases in `egl_draw_dmabuf_nv12`**

At the top of the function body:

```c
  if (zc_breakdown < 0)
    zc_breakdown = getenv("MOONLIGHT_ZC_BREAKDOWN") != NULL;
  double bd0 = zc_breakdown ? zc_now_ms() : 0.0;
```

Immediately after `dmabuf_get_nv12(...)` succeeds, capture the import time:

```c
  double bd1 = zc_breakdown ? zc_now_ms() : 0.0;
```

Just before `eglSwapBuffers`, optionally force GPU completion and capture the render time:

```c
  if (zc_breakdown) glFinish();
  double bd2 = zc_breakdown ? zc_now_ms() : 0.0;
```

After `eglSwapBuffers`:

```c
  double bd3 = zc_breakdown ? zc_now_ms() : 0.0;
  if (zc_breakdown) {
    zc_bd_img += bd1 - bd0;
    zc_bd_render += bd2 - bd1;
    zc_bd_swap += bd3 - bd2;
    if (++zc_bd_frames == 30) {
      fprintf(stderr, "EGL: zc breakdown img=%.2fms render+finish=%.2fms swap=%.2fms "
                      "creates=%u hits=%u\n",
              zc_bd_img / 30, zc_bd_render / 30, zc_bd_swap / 30,
              zc_img_creates, zc_img_hits);
      zc_bd_img = zc_bd_render = zc_bd_swap = 0;
      zc_bd_frames = 0;
    }
  }
```

The `glFinish` only runs under the env var, so normal behaviour is unchanged.

- [ ] **Step 4: build, then measure with the breakdown on**

```sh
scp F:/temp/wt-moonlight-drm/src/video/egl.c scy@172.30.71.3:~/Downloads/probe/moonlight-dev/src/video/egl.c
ssh scy@172.30.71.3 'sed -i "s/\r$//" ~/Downloads/probe/moonlight-dev/src/video/egl.c; cd ~/Downloads/probe/moonlight-dev && cmake --build build -j2'
```
Then run one real session with the breakdown (screen free; `ssh scy@172.30.71.3 'pgrep -a moonlight || echo none'`):
```sh
ssh scy@172.30.71.3 'DISPLAY=:0 LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=$HOME/Downloads/v4l2-dri MOONLIGHT_ZC_BREAKDOWN=1 timeout 45 $HOME/Downloads/probe/moonlight-dev/build/moonlight -platform x11_vaapi -codec h265 -1080 -fps 60 -localaudio -app Desktop stream 172.31.30.147 >/tmp/bd.log 2>&1 & sleep 40; timeout 8 $HOME/Downloads/probe/moonlight-dev/build/moonlight quit 172.31.30.147 >/dev/null 2>&1; pkill -x moonlight; grep -a "zc breakdown" /tmp/bd.log | tail -6'
```

- [ ] **Step 5: interpret**

Read the breakdown. Decision table:
- `creates` grows every frame and `hits` flat → import churn (fix the key).
- `render+finish` is the bulk (~20 ms) with `img`/`swap` small → a GPU-side stall on the imported buffer (fence/sync); next is a fence/wait investigation.
- `render+finish` small but `swap` large → the present path, not the shader.
- `img` large → `eglCreateImageKHR`/import cost.

- [ ] **Step 6: commit (diagnostic, kept behind the env var)**

```bash
git add src/video/egl.c
git commit -m "video/egl: MOONLIGHT_ZC_BREAKDOWN per-phase timing for the zero-copy path"
```

Then report the numbers and the conclusion. Task D2 (egl_bench A/B/C: cedrus dma-buf vs GBM BO vs `glTexSubImage2D`) is only started if D1 is inconclusive.
