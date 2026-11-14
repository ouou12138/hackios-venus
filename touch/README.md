# touch

Touchscreen for the Mi 11. Two controller paths are present: FocalTech (SPI) and
ST FTS. Touch input is verified on device.

## Files
- `src/redwood_goodix.c` — FocalTech FT3680 touch driver on the SoC SPI engine
  (GENI): clocks, pins, reset/IRQ, SPI FIFO, firmware load, touch reporting.
- `src/redwood_focaltech_fw.h` — FocalTech controller firmware (generated from
  `focaltech_ts_fw_m20.bin`); required by `redwood_goodix.c`, no device identity.
- `src/redwood_venus_st_protocol.c` — ST FTS touch protocol (the Mi 11's ST
  controller, `touchscreen=3`).
- `src/redwood_st_touch.h` — ST FTS protocol definitions / register map.
- `obj/redwood_goodix.o`, `redwood_venus_st_protocol.o` — compiled arm64 objects.

## Depends on
`common/redwood_input` (input core + logging), kernel GPIO / clock / SPI platform
layer.

## Status
Verified: touch down/move/up reported to iOS. Multi-finger / gesture edge cases
are not exhaustively verified.