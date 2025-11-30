# Project TODOs

This document tracks technical debt, required implementations, and future improvements for the Cat Tracker project.

## Tracker (Device A)

### Power Management
- [x] **Battery Monitoring:** Implemented `readBatteryLevel()` using `PIN_VBAT` (if available).
- [x] **Implement Deep Sleep:** `delay(15000)` calls `yield()` which enters System ON sleep. Radio and GPS are explicitly put to sleep.
    - *Decision:* `sd_power_system_off` (System OFF) disables RTC. Sticking to System ON.
- [x] **GPS Power Save:** Implemented `UBX-RXM-PMREQ` (Backup Mode) for BN-180 (u-blox).

### Radio & Communications
- [x] **Refactor RX Logic:** Cleanup `listenForDownlink` in Tracker. It now returns status (ACK/CMD) and uses `millis()` properly.
- [x] **Handle CMD_REPORT:** Receiving `CMD_REPORT` now triggers `performLocationUpdate()` immediately in the tracker loop.

## Gateway (Device B)

### Configuration & Credentials
- [x] **Externalize Secrets:** Credentials moved to `src/gateway/secrets.h`.
- [x] **Config Parsing:** Implement proper parsing for `CONFIG_UUID` downlink.

### Radio & Concurrency
- [x] **Non-blocking Loop:** Implemented interrupt-based `packetReceived` flag to allow `loop()` to run without blocking on `radio.receive()`.

### Mesh Logic
- [x] **Deduplication Cache:** Review `CACHE_SIZE`. Increased to 64.

## General / Common
- [x] **Testing:** Verify `PacketHeader` struct packing.
- [x] **Error Handling:** Add LED status indicators.
