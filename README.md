# hackios-venus

Driver source and compiled artifacts for running iOS on the **Xiaomi Mi 11**
(codename `venus`, Qualcomm SM8350 / Snapdragon 888, QCA6490 Wi‑Fi/BT).

This repository is a **driver-only extraction** from a larger XNU-based bring-up
tree. Each completed hardware driver lives in its own folder with its source
(`src/`) and its compiled arm64 Mach‑O object (`obj/`). A shared platform /
transport layer lives in `common/`, and kernel build scripts in `build/`.

> Scope: drivers only. The full iOS userland, the merged boot kernel, the
> bootloader, device firmware/calibration blobs and all private credentials are
> **not** included (see *What is excluded* below).

## Layout

| Folder        | Driver / component                    | Status on device |
|---------------|---------------------------------------|------------------|
| `battery/`    | Battery / fuel-gauge over ADSP GLINK  | Real charge/temp verified |
| `wifi/`       | QCA6490 Wi‑Fi (`venus_lmd` + ath11k)  | WPA2 + DHCP + DNS + HTTP/HTTPS verified |
| `bluetooth/`  | QCA6490 Bluetooth (H4 / native iOS)   | Controller + BLE scan verified; pairing/audio pending |
| `audio/`      | ADSP + CS35L41 speaker / mic          | Audible output verified (−24 dB cap) |
| `display/`    | Adreno GPU + DPU / 120 Hz framebuffer | 120 Hz present verified |
| `touch/`      | FocalTech / ST FTS touchscreen        | Touch input verified |
| `brightness/` | Backlight persistence (userland)      | Persist across reboot verified |
| `common/`     | Shared QMI / QRTR / ADSP / input / image layer | — |
| `build/`      | XNU kernel build scripts              | — |

Each folder has a `README.md` describing its files and how it fits the kernel.

## Source provenance

The drivers are written against Apple's XNU (`xnu-8796.121.2`) and the
`hackios-xnu-kernel` platform layer. They are original bring-up work for the
Mi 11, layered on top of APSL-licensed XNU. The Wi‑Fi path additionally builds
Linux's `ath11k` driver on a FreeBSD LinuxKPI compatibility layer (`wifi/lx-vlmd`);
see `NOTICE` for the third-party licenses that apply to those parts.

## Compiled artifacts

`obj/*.o` are the actual arm64 Mach‑O relocatable objects produced by the XNU
build, one per driver translation unit. They are the objects that get linked into
the kernel. To keep the public artifacts free of build-host paths, **debug
information has been stripped** (symbol tables and relocations are intact, so the
objects remain linkable). `wifi/obj/ath11k-vlmd.o` is the single partially-linked
object produced by `wifi/ath11k/mk-ath11k-vlmd.sh`.

These objects target the XNU kernel and are **not** standalone loadable modules;
they are linked into the kernel image by the XNU build (`build/xnu-build.sh`).

## What is excluded (sanitization)

To keep this repository safe to publish, the following were deliberately left out:

- **Private credentials** — Wi‑Fi SSID/passphrase files, `private/` directories,
  network profiles. The Wi‑Fi driver reads credentials from a ramdisk file at
  runtime and never logs them; no credential is present in this repo.
- **Device identity** — serial / MLB / UDID / ECID / MAC values and the bootloader
  identity spoofing. No device identifier is included.
- **Firmware & calibration blobs** — the Wi‑Fi BDF (board data, contains MAC/RF
  calibration), AMSS/M3/BHI firmware images, and the old experimental Wi‑Fi track
  (`redwood_wifi`) that embedded them. The included `venus_lmd` Wi‑Fi driver loads
  firmware from the device at runtime and embeds none.
- **The merged boot kernel** — it links the old Wi‑Fi object that bundles the
  BDF/firmware, so it is not published. Per-driver objects are provided instead.
- **The iOS userland / root filesystem** and all research logs.

The only binary blob retained is `touch/src/redwood_focaltech_fw.h` (touch-panel
controller firmware), which the touch driver `#include`s and which contains no
device identity.

## Building

The objects here were produced by the XNU cross-build on Linux/WSL. See
`build/xnu-build.sh` and `build/xnu-env.sh`. Rebuilding requires the full XNU
source tree and toolchain (not part of this driver-only repo). The Wi‑Fi radio
object is rebuilt with `wifi/ath11k/mk-ath11k-vlmd.sh`.

## License

Base XNU/platform code is under the Apple Public Source License 2.0 (`LICENSE`).
Third-party components (Linux ath11k, FreeBSD LinuxKPI) retain their own licenses;
see `NOTICE`.