# audio

Audio for the Mi 11: SoC ADSP (Qualcomm DSP) plus the CS35L41 speaker amplifier,
exposed to iOS CoreAudio. Digital audio path and physical speaker output are
verified.

## Files
- `src/redwood_audio.c` / `redwood_audio.h` — CoreAudio-facing driver, HAL glue.
- `src/redwood_sound.c` — sound / DMA path.
- `src/redwood_speaker.c` — CS35L41 speaker amplifier (OTP/HALO, TDM, gain).
- `src/redwood_mic.c` — microphone path.
- `src/redwood_audio_volume.inc` — volume / gain tables.
- `src/redwood_venus_cs35_output.inc`, `redwood_venus_cs35_power.h` — CS35L41
  output configuration and power sequencing.
- `obj/redwood_audio.o`, `redwood_sound.o`, `redwood_speaker.o`, `redwood_mic.o`
  — compiled arm64 objects.

## Depends on
`common/redwood_adsp` (ADSP loader / APR), `common/redwood_qmi`.

## Status
Verified: audible playback on the physical speaker (HALO 96 kHz, stock CS35
params), zero write errors / underruns. A −24 dB output cap is retained.
Long-term / full-media quality is not fully verified.