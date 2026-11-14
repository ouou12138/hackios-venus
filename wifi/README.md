# wifi

QCA6490 Wi‑Fi for the Mi 11, built as the **`venus_lmd`** port. This is the
working Wi‑Fi path: it brings the radio up over MHI, talks QMI/QRTR to the
firmware, and runs Linux's `ath11k` driver on a FreeBSD LinuxKPI compatibility
layer. WPA2 association, DHCP, DNS and HTTP/HTTPS are verified on device.

> The older experimental `redwood_wifi` track (which embedded firmware/BDF blobs)
> is **not** included here.

## Kernel driver (`src/`)
- `venus_lmd_wifi.c` / `.h` — top-level driver: power/clock bring-up, MHI, QMI
  capability exchange, firmware memory grant, radio start.
- `venus_lmd_radio.c` / `.h` — radio supply sequence / WLFW service.
- `venus_lmd_qrtr.c` / `.h` — QRTR router over the IPCR transport.
- `venus_lmd_qmi.h` — QMI service definitions.
- `venus_lmd_mhi_protocol.h` — MHI protocol structures.
- `venus_lmd_lx.c` / `.h` — bridge to the LinuxKPI layer; reads the network
  name/passphrase from a ramdisk file at runtime (never logged).
- `redwood_lx.c` / `.h` — kernel-side bus/OS glue the LinuxKPI layer calls.
- `redwood_net.c` / `.h`, `redwood_sta.c` / `.h` — network interface / STA glue.
- `redwood_apple80211.c`, `redwood_wlfw.c` — Apple80211 / WLFW interface.
- `RedwoodWLAN.cpp`, `MiniIONetworkStack.cpp` — IOKit WLAN / network stack.

## LinuxKPI layer (`lx-vlmd/`)
Original headers + OS glue (`src/`, `include/`) and FreeBSD LinuxKPI header
patches (`patches/`) that let unmodified Linux `ath11k` compile for XNU.

## ath11k build (`ath11k/`)
- `mk-ath11k-vlmd.sh` — fetches pinned Linux + FreeBSD, applies the patches,
  compiles `ath11k` and partially links it into one object.

## Compiled objects (`obj/`)
`venus_lmd_*.o`, `redwood_lx.o`, `redwood_net.o`, `redwood_sta.o`,
`redwood_apple80211.o`, `redwood_wlfw.o`, and `ath11k-vlmd.o` (the radio object).

## Depends on
`common/redwood_qmi`, `common/redwood_qrtr`, `common/redwood_image` (firmware
file loading). Firmware (AMSS, board data, M3) is loaded from the device at
runtime and is **not** bundled.

## Status
Verified: scan → auth → assoc → WPA2 4-way handshake → DHCP → IPv4 → DNS →
HTTP/HTTPS, reconnect, and throughput (~92 Mbps up on 5 GHz).