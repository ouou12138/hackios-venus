# bluetooth

QCA6490 Bluetooth for the Mi 11. Brings the controller up over UART (H4), loads
the phone's own patch/NVM firmware at runtime, and hands the HCI transport to the
native iOS Bluetooth stack.

## Files
- `src/venus_bluetooth.c` — kernel driver: UART / clocks / pins, controller
  bring-up, HCI reset, firmware download, H4 queue.
- `src/venus_bt_protocol.h`, `venus_bt_queue.h`, `venus_bt_native.h` — H4 / HCI
  protocol and queueing.
- `src/venus_bt_firmware.h` — TLV validation for the controller firmware inputs
  (no firmware bytes, addresses or keys are stored here).
- `src/venus_bluetooth.h` — public interface.
- `src/VenusBluetooth.cpp` — IOKit Bluetooth nub for the native iOS stack.
- `obj/venus_bluetooth.o` — compiled arm64 object.

## Depends on
`common/redwood_input` (logging), GPIO/clock helpers in the kernel platform layer.

## Status
Verified: controller identified (QCA6490), HCI reset + local address readback,
passive BLE advertising reception, classic inquiry events, native iOS stack
takeover. Pairing / reconnect / HID / audio / sleep are **not yet verified**.
Controller firmware is loaded from the device and is not bundled.