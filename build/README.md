# build

Kernel build scripts used to compile the drivers into XNU.

## Files
- `xnu-build.sh` — invokes the XNU `make` for the `redwood` target (ARM64,
  DEVELOPMENT config), linking the Wi‑Fi radio object when present.
- `xnu-env.sh` — toolchain / environment setup sourced by `xnu-build.sh`.

## Notes
- These scripts expect the **full XNU source tree and cross toolchain** (clang /
  ld64, host tools), which are not part of this driver-only repository. They are
  included to document how the objects in each `obj/` folder were produced.
- The Wi‑Fi radio object is built separately by `wifi/ath11k/mk-ath11k-vlmd.sh`
  and linked in via `REDWOOD_OBJECTS`.
- The merged boot kernel is intentionally not published (it bundles the old
  Wi‑Fi object's firmware/BDF); per-driver objects are provided instead.