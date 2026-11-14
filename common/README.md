# common

Shared platform / transport layer used by several drivers. These are not
peripheral drivers themselves but the bus / service glue the drivers link against.

## Files
- `src/redwood_qmi.c` / `.h` — QMI (Qualcomm MSM Interface) transport, used by
  Wi‑Fi, audio (ADSP) and battery.
- `src/redwood_qrtr.h` — QRTR (QMI router) definitions.
- `src/redwood_adsp.c` — ADSP remoteproc loader / GLINK / APR channel, shared by
  audio and battery.
- `src/redwood_image.c` — firmware / image file loading from the ramdisk.
- `src/redwood_input.c` / `.h` — input subsystem core and the `redwood_tl`
  telemetry/log function used across all drivers.
- `src/MiniDrivers.h` — shared IOKit driver declarations.
- `obj/redwood_qmi.o`, `redwood_adsp.o`, `redwood_image.o`, `redwood_input.o`
  — compiled arm64 objects.

## Consumers
`wifi/`, `audio/`, `battery/`, `bluetooth/`, `touch/`, `display/`.