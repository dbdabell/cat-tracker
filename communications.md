# Communications Specification

## Overview

This document outlines the communication protocols and requirements for the Cat Tracker system. The system consists of two primary node types:
1.  **Tracker (Device A):** A wearable device attached to the subject.
2.  **Gateway (Device B):** A stationary node that bridges the LoRa mesh network to the internet (Traccar server).

The system utilizes a **Hybrid Mesh Network** where gateways can act as repeaters to extend range, ensuring coverage even in areas without direct WiFi or cellular access.

## Physical Layer

### Radio Protocol (LoRa)
*   **Hardware:** Semtech SX1262 (Tracker) and SX1262/SX1276 (Gateway).
*   **Library:** RadioLib.
*   **Frequency:** 915 MHz (or regional equivalent).
*   **Modulation:** LoRa (Long Range) - optimized for low power and long distance.

### Bluetooth Low Energy (BLE)
*   **Role:** Proximity detection.
*   **Mechanism:** Gateways broadcast a specific **Beacon UUID**.
*   **Tracker Behavior:** The tracker scans for this UUID to determine if it is "Home" (safe zone) or "Roaming".

---

## Data Protocol

All data is transmitted using a custom binary packet structure defined in `common.h`. This structure supports mesh networking capabilities like store-and-forward and deduplication.

### Packet Structure
Every packet begins with a standard header:

| Field | Type | Description |
| :--- | :--- | :--- |
| `deviceID` | `uint32_t` | Unique identifier of the sender (Tracker ID). |
| `messageID` | `uint8_t` | Random 8-bit ID for deduplication (prevents broadcast storms). |
| `hopCount` | `uint8_t` | Time-To-Live (TTL). Starts at 3. Decremented by repeaters. |
| `packetType` | `uint8_t` | Defines the payload type (see below). |

### Packet Types

| Type ID | Name | Description | Payload |
| :--- | :--- | :--- | :--- |
| `0x01` | **LOCATION** | GPS coordinates and battery status. | `lat` (float), `lon` (float), `battery` (uint8_t) |
| `0x02` | **HEARTBEAT** | Status update when "Home" or no GPS fix. | None (Header only) |
| `0x10` | **CMD_REPORT** | Command from Gateway to Tracker to force update. | None |
| `0x20` | **CONFIG_UPDATE** | Update configuration (e.g., Home Beacon UUIDs). | `count` (uint8_t), `uuids` (16-byte arrays) |

---

## Operational Flows

### 1. Uplink: Tracker to Gateway
The tracker initiates communication based on its state:

*   **Home Mode (BLE Beacon Detected):**
    *   **Action:** Tracker suppresses GPS to save power.
    *   **Message:** Sends `HEARTBEAT` packet.
    *   **Frequency:** Periodic (low frequency).

*   **Roaming Mode (No Beacon):**
    *   **Action:** Tracker activates GPS.
    *   **Message:** Sends `LOCATION` packet upon GPS fix.
    *   **Fallback:** Sends `HEARTBEAT` if no GPS fix is available after timeout.

### 2. Mesh Networking (Repeater Function)
Gateways act as intelligent repeaters to extend the network:

1.  **Receive:** Gateway receives a LoRa packet.
2.  **Deduplicate:** Checks `messageID` against a cache of recently seen packets. If seen, the packet is ignored.
3.  **Forwarding Logic:**
    *   **Online:** If the Gateway has WiFi, it uploads the data to the Cloud (Traccar). It does *not* repeat the packet via LoRa to reduce congestion.
    *   **Offline:** If the Gateway has no WiFi, it checks `hopCount`.
        *   If `hopCount > 0`, it decrements `hopCount`, waits a random interval (Collision Avoidance), and re-transmits the packet via LoRa.
        *   This allows a chain of offline gateways to reach an online gateway.

### 3. Downlink: Gateway to Tracker
Gateways can broadcast commands to Trackers (e.g., from the Cloud user interface):

*   **Commands:** `CMD_REPORT` (Force GPS read), `CONFIG_UPDATE` (Change Home UUID).
*   **Mechanism:** Commands are flooded through the mesh (Store & Forward) to ensure they reach the Tracker even if it is out of direct range of the primary gateway.
*   **Reception:** Trackers listen for a short window after transmission or wake up periodically to check for downlink messages.

---

## Cloud Integration (Gateway -> Traccar)

The Gateway bridges the LoRa mesh to the Traccar server using HTTP GET requests.

*   **Protocol:** OsmAnd protocol (Traccar compatible).
*   **Format:**
    ```
    http://<server>:5055/?id=<deviceID>&lat=<lat>&lon=<lon>&batt=<battery>
    ```

