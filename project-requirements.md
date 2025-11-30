Project: "Atomic" Scalable Cat Tracker System (Mesh Edition)

1. Role & Objective

Act as a Senior Embedded Systems Architect. Scaffold a full PlatformIO project for a dual-node LoRa tracking system. The system must support Mesh Networking (Store & Forward) to allow offline gateways to repeat signals to online gateways.

2. Hardware Context

Device A: The Tracker (Wearable)

Target Board: Seeed XIAO nRF52840.

Radio: Semtech SX1262 (SPI via EByte E22-900M22S).

GPS: Beitian BN-180 (UART).

Power: 3.7V LiPo Battery (400mAh).

Identity: Silicon-derived Unique Serial Number (uint32_t).

Device B: The Gateway (Home Station / Repeater)

Target Board: Heltec WiFi LoRa 32 V3 (ESP32-S3).

Features: Integrated OLED, LoRa, Wi-Fi, BLE Beacon.

Identity: Pass-through gateway with auto-repeater function.

3. Data Protocol (Shared common.h)

3.1 Packet Structure (Mesh Enabled)

All packets start with a standard header.

struct PacketHeader {
    uint32_t deviceID;     // Unique Sender ID
    uint8_t  messageID;    // Random uint8 to identify unique packets (Deduplication)
    uint8_t  hopCount;     // TTL (Time To Live), starts at 3
    uint8_t  packetType;   // 0x01=Location, 0x02=Heartbeat, 0x10=Cmd_Report
};

struct LocationPayload {
    float lat;
    float lon;
    uint8_t battery;
};

struct ConfigPayload {
    uint8_t count;
    uint8_t uuids[5][16]; 
};


4. Functional Requirements

4.1 Tracker Firmware Logic (Device A)

Setup: Initialize LittleFS, Radio, and BLE Scanner. Load Config. Generate deviceID.

FSM (Home/Roaming):
1. Wake up.
2. Scan for Gateway BLE Beacon (1-2 seconds).
3. If Beacon Found (Home Mode):
   - Set internal state to "Home".
   - Skip GPS acquisition.
   - Send Heartbeat Packet (Type 0x02) to confirm presence.
4. If Beacon Not Found (Roaming Mode):
   - Enable GPS.
   - Wait for Fix (timeout 30s).
   - If Fix: Send Location Packet (Type 0x01).
   - If No Fix: Send Heartbeat (Type 0x02).

Transmission: When sending ANY packet, generate a random messageID and set hopCount = 3.

RX Window: After transmission (or periodic wake-up), listen for Downlink Packets (2 seconds).
   - If CMD_REPORT received: Trigger Immediate Location Update.
   - If CONFIG_UPDATE received: Update Target UUIDs in LittleFS.

4.2 Gateway Firmware Logic (Device B)

Global Cache: Maintain a RecentMessages list (last 20 messageIDs) to prevent broadcast storms.

Location History: Maintain a persistent rolling history of location data in flash memory.

- Storage: Up to 1000 location entries stored in LittleFS filesystem
- Persistence: History is automatically saved to flash every 10 entries to reduce wear
- Circular Buffer: When capacity is reached, oldest entries are automatically overwritten
- Cleanup: On startup, entries older than 7 days are automatically removed
- Memory: Each entry contains deviceID, latitude, longitude, battery level, and timestamp
- Storage Size: Approximately 20KB for full history (1000 entries × 20 bytes)

Packet Handler:

On Receive -> Check RecentMessages. If exists, ignore.

Add messageID to cache.

If Location Packet (Type 0x01):
   - Always save to location history (regardless of WiFi status)
   - Check Wi-Fi Status.
   - If Wi-Fi Connected: Upload to Traccar via HTTP.
   - If Wi-Fi Disconnected: Check hopCount. If > 0, decrement hopCount, wait random delay (50-200ms), retransmit Packet (LoRa TX).

If Other Packet Types:
   - If Wi-Fi Disconnected: Check hopCount. If > 0, decrement hopCount, wait random delay (50-200ms), retransmit Packet (LoRa TX).

Downlink (Mesh Command):

If "REPORT_NOW" received from Traccar (by an Online Gateway):

Broadcast CMD_REPORT with hopCount = 3.

If "CONFIG_UUID" received from Traccar:
Broadcast PACKET_TYPE_CONFIG_UPDATE with new UUID.

Offline gateways will hear this and repeat it, extending the range of the command to the remote reaches of the property.

5. Technical Stack

Framework: Arduino (via PlatformIO).

Libraries:

RadioLib (LoRa).

TinyGPSPlus (GPS).

Adafruit Bluefruit nRF52 (Tracker BLE).

heltecautomation/Heltec ESP32 Dev-Boards (Gateway).

LittleFS_esp32 (Gateway persistent storage).

6. Deliverables Checklist

platformio.ini: Configured for xiao_nrf52840 and heltec_wifi_lora_32_V3.

include/common.h: The shared struct definition with Mesh Headers.

src/tracker/main.cpp: Source code for Tracker.

src/gateway/main.cpp: Source code for Gateway (Implementing Mesh/Repeater logic).