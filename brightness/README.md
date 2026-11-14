# brightness

Backlight control and persistence for the Mi 11. The kernel-side DCS brightness
hook lives in `display/src/MiniIOMobileFramebuffer.cpp` (with a panel-level max
guard so auto-brightness cannot demand full scale). This folder holds the
**userland persistence** side: it stores the user's brightness and restores it
across reboots.

## Files
- `src/venus_brightness_state.h` — brightness state model.
- `src/venus_brightness_store.h` — persistence interface.
- `src/var_store.c` — variable store backing the persisted brightness value.

## Notes
- This is userland code (no kernel `.o`); it is compiled into the runtime, not the
  kernel. The kernel brightness hook is part of the `display/` driver.
- A max-brightness guard (`venus_bl_max`) clamps the DCS value so the panel is not
  driven to full scale on boot/wake without an ambient-light sensor.

## Status
Verified: brightness persists across reboot (restores the saved level), slider
adjusts the panel, no full-scale command on boot.