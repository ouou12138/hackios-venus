# battery

Battery / fuel-gauge driver for the Mi 11. Real charge level and temperature are
read from the PMIC fuel gauge over the ADSP GLINK channel (the SoC's DSP reports
the battery telemetry; the kernel samples it).

## Files
- `src/redwood_battery.c` — kernel battery driver: ADSP telemetry sampling,
  charge/temperature reporting, power-supply properties.
- `src/RedwoodBattery.cpp` — IOKit battery nub exposed to iOS power management.
- `obj/redwood_battery.o` — compiled arm64 object.

## Depends on
`common/redwood_adsp` (ADSP loader / GLINK transport) and `common/redwood_qmi`.

## Status
Verified on device: real charge percentage and temperature match Android.