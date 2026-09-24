# Fixed Cedar decoder ownership audit

2026-09-24. Scope: pinned `243f2cbe` CedarC source and its
`library/aarch64-none-linux-gnu` blobs. This is source/binary evidence for the
production decoder design, **not a successful production device cleanup test**.

## SBM is owned by VideoEngine after attachment

The public `vdecoder/vdecoder.c:432` destructor does not directly call
`SbmDestroy`. That alone does **not** imply an SBM leak. The fixed
`libvideoengine.so` (SHA256
`abfea1fa75f6a3ca5413d16eb69fcb87692f3e3b6552d83a222a998e909cef82`)
contains this ownership chain:

- `VideoEngineSetSbm` at `0x3404`: `0x3448` computes engine + index * 8;
  `0x344c` stores the supplied SBM pointer at offset 496 (index 0) / 504 (1).
- `VideoEngineDestroy` at `0x2e34`: first invokes the decoder interface's
  callback at +48 (`0x2e70..0x2e78`, matching `DecoderInterface.Destroy`).
- At `0x2e7c..0x2e98` it loads engine offsets 496 and 504 and, for each non-null
  SBM, calls its function pointer at offset +8. In the matching public
  `SbmInterface`, +8 is `destroy` on this 64-bit ABI.
- `CreateH264Decoder` installs the callback at +48 as `0xf7d0`, identified by
  the blob's local symbol as `H264DecoderDestroy`. This callback invokes
  `H264FreeMemory`; this audit does not enumerate every blob-internal allocation.

Reproduce with GNU AArch64 objdump against the pinned files:

```sh
aarch64-linux-gnu-objdump -d --disassemble=VideoEngineSetSbm library/aarch64-none-linux-gnu/libvideoengine.so
aarch64-linux-gnu-objdump -d --disassemble=VideoEngineDestroy library/aarch64-none-linux-gnu/libvideoengine.so
aarch64-linux-gnu-objdump -d --disassemble=CreateH264Decoder library/aarch64-none-linux-gnu/libawh264.so
aarch64-linux-gnu-objdump -d --start-address=0xf7d0 --stop-address=0xf960 library/aarch64-none-linux-gnu/libawh264.so
```

The selected frame SBM's source destructor
`vdecoder/sbm/sbmFrameBase.c:SbmFrameDestroy` posts QUIT and joins the parser
thread before freeing its stream allocation via `CdcMemPfree`. The selected
build has `ENABLE_NEW_MEMORY_OPTIMIZATION_PROGRAM=0`; its primary stream ring
is allocated through the supplied memory adapter, not ordinary malloc.
`DestroyVideoDecoder` calls `VideoEngineDestroy` before `CdcMemClose` and
`CdcVeRelease`. This supports checking that the production adapter is empty
after a healthy, fully initialized decoder is destroyed. It does not justify
force-freeing residual allocations.

## Failure and concurrency caveats still required in production

1. `InitializeVideoDecoder` is not transactional: its error exit does not
   unwind every resource acquired earlier. An initialized-success cleanup
   argument cannot be applied to arbitrary initialization failures.
2. SBM QUIT posting and pthread join return values are not checked by the
   vendor destructor. There is no proven bounded cleanup under arbitrary
   failure. Do not kill a worker and claim resources safely retired.
3. `SubmitVideoStreamData` discards the underlying `SbmAddStream` result.
   A public successful submission is not decoded-frame or presentation proof.
   Bound compressed input, preserve complete AU boundaries, and count actual
   output pictures separately.
4. The memory adapter's void callbacks record a sticky first error. Inspect
   `memory_status` after Cedar calls, not only their immediate return values.
5. A DMA allocation pin prevents deallocation, not decoder reuse after
   `ReturnPicture`. The decoder must retain each picture until the consumer
   explicitly retires it; a display consumer needs actual retirement evidence.
   In `fbm.c:FbmReturnPicture`, successful return clears `bUsedByRender` and
   decrements `nRenderHoldingNum`; if the decoder does not hold the picture,
   it is immediately enqueued for reuse (or release). Thus even a live fd and
   an allocation pin cannot substitute for retaining the actual picture.
6. Reset/destroy must not run with externally held pictures. One controlled
   owner thread drives Cedar; the asynchronous SBM thread is internal to Cedar.
7. Healthy teardown must establish: no external picture leases; destroy
   decoder; adapter references/allocations/pins/quarantines all zero; only then
   `memory_end`. Unexpected or ambiguous state stops further device operations,
   without speculative repeated ENGINE_REL or forced cleanup.

## Old probe evidence is contextual, not production verification

On the board, the existing `cedarx_test/logs/tina-verify600.log`,
`final-bench2.log` and `final-sigterm.log` were inspected read-only. Their
reported output counts are respectively 600, 600 and 89 pictures; none contains
the old adapter's `Cedar memory close: releasing residual buffers` warning.
That absence is consistent with ordinary cleanup but is not an allocation
ledger or proof that the new production adapter works on hardware. The old
adapter's fallback loop must not be copied into production.

The next production stage is an owner-thread decoder with bounded compressed
input, explicit picture leases and the strict picture-to-frame converter,
followed by the fixed sample's per-frame pixel verification. Dynamic disp
retirement and actual Sunshine 1080p60 remain separate unfinished requirements.
