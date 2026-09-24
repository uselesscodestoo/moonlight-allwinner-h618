# Cedar picture to NV12 descriptor implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Translate an actually retained Cedar `VideoPicture` and two memory-adapter allocation views into the existing zero-copy display descriptor, rejecting unsupported or inconsistent layouts.

**Architecture:** A pure metadata function, with no Cedar calls, device access, pixel reads/copies, pinning or ownership transfer. The subsequent decoder owner must retain the picture, obtain both views from `memory_describe`, check memory status and pin it before publishing; descriptor validation never authorizes `ReturnPicture` or proves display retirement.

**Tech Stack:** C99, real fixed Cedar headers (`TINA_LINUX_SUPPORT=0`), existing `frame.c`, Make unit tests and private runtime CMake target.

This is one dependency of the approved full backend design, not replacement acceptance. Decoder lifecycle/worker, actual production sample decode, display retirement and real Sunshine 1080p60 remain required and unfinished.

## Task 1: Pure picture layout conversion

**Files:** Create `src/video/k2b/cedar_picture.h`, `src/video/k2b/cedar_picture.c`, `tests/k2b/test_cedar_picture.c`; modify `tests/k2b/Makefile` and `cmake/K2BCedarRuntime.cmake`.

- [x] Write the public contract and failing tests using real headers, no replacement Cedar structs:

```c
int k2b_cedar_picture_frame(const VideoPicture *picture,
    const struct k2b_cedar_buffer *y, const struct k2b_cedar_buffer *uv,
    enum k2b_matrix matrix, enum k2b_range range, struct k2b_frame *out);
```

Returns 0 or -1/EINVAL; failure leaves `out` unchanged. Inputs must remain stable under external picture ownership. `matrix`/`range` come from negotiated stream metadata, not Cedar's potentially unset color fields. This function does not inspect the fd's OS validity.

Use a real static byte array at least `1920*1088*3/2+4096` bytes for the ordinary fixture, set `pData0` to its base, `pData1` to base + `1920*1088`, fd 7 and identical allocation/VE-base identity in both views. `VideoPicture` is zero-initialized, NV12, stream 0, progressive 1, width/stride 1920, height 1088, right/bottom 1920/1080, `nBufSize=1920*1088*3/2`. Example test core:

```c
struct k2b_frame out, before;
memset(&out, 0xa5, sizeof(out));
before = out;
CHECK(k2b_cedar_picture_frame(&pic, &y, &uv,
    K2B_MATRIX_BT709, K2B_RANGE_LIMITED, &out) == 0);
CHECK(out.fd == 7 && out.uv_offset == 1920u*1088u);
CHECK(out.width == 1920 && out.height == 1080 && out.storage_height == 1088);
pic.pData1++;
out = before;
errno = 0;
CHECK(k2b_cedar_picture_frame(&pic, &y, &uv,
    K2B_MATRIX_BT709, K2B_RANGE_LIMITED, &out) == -1);
CHECK(errno == EINVAL && memcmp(&out, &before, sizeof(out)) == 0);
```

Tests must remain live with NDEBUG (`CHECK`, not assert). Cover every guard independently: each null argument; NV21/other format; stream other than zero; non-progressive; each frame/field-error flag; 10-bit/AFBC; third/fourth plane; negative/zero/over-limit storage width/height/stride; width greater than stride; negative/inverted/out-of-storage crops; wrong visible dimensions; odd crop/stride/height; both color matrices/ranges and invalid enums; null/different allocation base, different fd/size/VE base; negative fd including accepting fd 0; nonzero Y offset; out-of-allocation UV; wrong UV offset and pointer; truncated allocation; `nBufSize` too small/negative/larger than allocation; address range overflow. Exercise valid padded stride/cropped 1920x1080 in larger even storage, with matching offsets. Set picture color fields to unrelated values and verify negotiated values survive. No pixels may be accessed even for synthetic integer-address overflow fixtures.

- [x] Run RED: `make -f tests/k2b/Makefile test-picture K2B_CEDARC_ROOT=/mnt/f/temp/projects/cedarx_test/libcedarc-tina`; absence/stub must fail. Record output before implementation.
- [x] Implement the converter with a local result then publish only on success:

```c
/* Bounds before subtraction/multiplication; no pointer subtraction across objects. */
if (!picture || !y || !uv || !out) goto invalid;
if (picture->ePixelFormat != PIXEL_FORMAT_NV12 || picture->nStreamIndex != 0 ||
    picture->bIsProgressive != 1 || picture->b10BitPicFlag || picture->bEnableAfbcFlag ||
    picture->bFrameErrorFlag || picture->bTopFieldError || picture->bBottomFieldError ||
    picture->pData2 || picture->pData3) goto invalid;
if (picture->nWidth <= 0 || picture->nWidth > 8192 ||
    picture->nHeight <= 0 || picture->nHeight > 8192 ||
    picture->nLineStride <= 0 || picture->nLineStride > 8192 ||
    picture->nWidth > picture->nLineStride ||
    picture->nLeftOffset < 0 || picture->nTopOffset < 0 ||
    picture->nRightOffset < picture->nLeftOffset ||
    picture->nBottomOffset < picture->nTopOffset ||
    picture->nRightOffset > picture->nWidth ||
    picture->nBottomOffset > picture->nHeight) goto invalid;
if (!y->base || y->base != uv->base || y->fd < 0 || y->fd != uv->fd ||
    y->bytes != uv->bytes || y->ve_address != uv->ve_address ||
    y->offset != 0 || uv->offset >= uv->bytes ||
    y->bytes > UINTPTR_MAX - (uintptr_t)y->base ||
    (uintptr_t)picture->pData0 != (uintptr_t)y->base ||
    (uintptr_t)picture->pData1 != (uintptr_t)y->base + uv->offset) goto invalid;
struct k2b_frame frame = {0};
frame.fd = y->fd;
frame.format = K2B_PIXEL_NV12;
frame.allocation_bytes = y->bytes;
frame.y_offset = y->offset;
frame.uv_offset = uv->offset;
frame.stride = (uint32_t)picture->nLineStride;
frame.storage_height = (uint32_t)picture->nHeight;
frame.crop_x = (uint32_t)picture->nLeftOffset;
frame.crop_y = (uint32_t)picture->nTopOffset;
frame.width = (uint32_t)(picture->nRightOffset - picture->nLeftOffset);
frame.height = (uint32_t)(picture->nBottomOffset - picture->nTopOffset);
frame.matrix = matrix;
frame.range = range;
if (k2b_frame_validate(&frame) != 0) goto invalid;
size_t used = frame.uv_offset + frame.uv_offset / 2;
if (picture->nBufSize < 0 || (size_t)picture->nBufSize < used ||
    (size_t)picture->nBufSize > y->bytes) goto invalid;
*out = frame;
return 0;
invalid:
errno = EINVAL;
return -1;
```

Header documents borrowing, external synchronization and no pixel access. Reuse `k2b_frame_validate` for NV12 offset/extent/evenness, rather than duplicate its rules. Avoid comparing or exporting VE IOVA as a display address. `nBufFd` is not used as allocation authority; use adapter view fd (test this explicitly).

- [x] Add `test-picture` following `test-runtime`'s real-header includes and order-only phony header checks, so a cached binary cannot hide missing headers. New test executable compiles `cedar_picture.c` and `frame.c`, no runtime library/device dependency. Add both sources to existing static `k2b_cedar_runtime` target so production native/cross build compiles the code. Do not link vendor blobs into the loader or alter OFF baseline.
- [x] GREEN: run ordinary, NDEBUG and ASan+UBSan tests with separate BUILD_DIR values; all return 0. Run `test`, `test-au`, `test-queue`, `test-runtime` regressions. Check missing-header rejection with a cached `test-picture` binary. Rebuild existing cross private runtime, not executing AArch64 code on x86.
- [x] Commit only these five files. Spec review first, then quality review; address findings before proceeding.

## Task 2: Board verification and evidence (parent)

**Files:** Modify `docs/k2b-bringup.md`, this checklist; create `docs/k2b-decoder-lifecycle-audit.md` recording the fixed-binary SBM ownership evidence.

- [x] Record source/binary evidence: `VideoEngineSetSbm` stores SBM at offsets 496/504; `VideoEngineDestroy` at 0x2e7c..0x2e98 invokes each object's callback at +8, which matches `SbmInterface.destroy`. Do not claim this proves failed-initialization rollback or live device cleanup.
- [x] Verify both repositories clean and expected ancestry, transfer a git bundle over SSH to `kickpi@172.31.197.223`, hash-check and fast-forward only. Do not copy cross-compiled libraries or change network authentication.
- [x] On board run ordinary/NDEBUG `test-picture` with existing fixed managed source and rebuild native private runtime with `cmake --build build/k2b-runtime-native --parallel 2`. No VPU initialization or HDMI action in this metadata test. Save full output locally.
- [x] Record exact outcomes, source commits, current address and next unfinished decoder ownership/worker stage. Keep full-goal status active; this helper is not a hardware decode/display result.

Self-review: matches approved strict NV12 layout and negotiated color requirements; all new types exist in fixed headers. This plan deliberately covers only picture-layout conversion; runtime ownership and live display retirement are still separate required implementation stages, not claimed by unit-test success.

Completion evidence: implementation `e2737c9`, isolated width-boundary test
`f498bf7`; spec and quality reviews passed, including a mutation check proving
the added test fails when only the relevant guard is removed. Board native
ordinary/NDEBUG each passed 319 checks, private runtime rebuilt successfully.
Commands, scope limits and log hash are recorded in `docs/k2b-bringup.md`.
