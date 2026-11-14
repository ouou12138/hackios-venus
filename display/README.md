# display

Adreno GPU + display pipeline for the Mi 11, driving the panel at **120 Hz**.
Covers the GPU (Adreno 660), the DPU / framebuffer, IOSurface, and the refresh /
timing control that produces a real 120 Hz present.

## Files
- `src/redwood_adreno.c` — Adreno GPU driver: SMMU page tables, GMU/power,
  ringbuffer, context management.
- `src/RedwoodGPU.cpp` — IOKit GPU nub.
- `src/MiniIOMobileFramebuffer.cpp` — framebuffer / display controller (also
  hosts the brightness DCS hook used by `brightness/`).
- `src/MiniIOSurface.cpp` / `.h` — IOSurface (shared buffers).
- `src/MiniFramebufferFlip.inc` — page-flip / double-buffer logic.
- `src/VenusDpu.inc` — DPU (display processing unit) configuration.
- `src/VenusRefresh.inc`, `VenusRefreshStock.inc` — 120 Hz refresh / TE timing.
- `src/GenericARMPE.cpp` — ARM platform expert glue (device identity / model).
- `obj/redwood_adreno.o` — compiled arm64 object.

## Depends on
`common/redwood_input` (logging), kernel SMMU / clock / power platform layer.

## Status
Verified: panel TE ~120.7 Hz, DPU advances, native UIKit 120 Hz, ~120 FPS
present, QSEED scaling + double-buffer. Per-pixel / no-tear and all-app 120 Hz
are not fully verified.