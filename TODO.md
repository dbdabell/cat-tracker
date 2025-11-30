# Project TODOs

This document tracks technical debt, required implementations, and future improvements for the Cat Tracker project.

## Tracker (Device A)

### Power Management
- [x] **Battery Monitoring:** Implemented `readBatteryLevel()` using `PIN_VBAT` (if available).
- [ ] **Implement Deep Sleep:** `delay(15000)` calls `yield()` which enters System ON sleep (low power). 
    - *Improvement:* Verify if `sd_power_system_off` (Deep Sleep) is feasible with RTC wake-up for even lower consumption (<1uA vs ~2uA).

### Radio & Communications
- [ ] **Refactor RX Logic:** Cleanup `listenForDownlink` in Tracker. It currently uses a blocking delay/loop.
- [ ] **Handle CMD_REPORT:** Ensure that receiving `CMD_REPORT` actually triggers a fresh GPS read.

## Gateway (Device B)

### Configuration & Credentials
- [x] **Externalize Secrets:** Credentials moved to `src/gateway/secrets.h`.
- [ ] **Config Parsing:** Implement proper parsing for `CONFIG_UUID` downlink.

### Radio & Concurrency
- [x] **Non-blocking Loop:** Implemented interrupt-based `packetReceived` flag to allow `loop()` to run without blocking on `radio.receive()`.

### Mesh Logic
- [ ] **Deduplication Cache:** Review `CACHE_SIZE`.

## General / Common
- [ ] **Testing:** Verify `PacketHeader` struct packing.
- [ ] **Error Handling:** Add LED status indicators.
