#ifndef COMMON_H
#define COMMON_H

#include <Arduino.h>

// Packet Types
#define PACKET_TYPE_LOCATION      0x01
#define PACKET_TYPE_HEARTBEAT     0x02
#define PACKET_TYPE_ACK           0x03
#define PACKET_TYPE_CMD_REPORT    0x10
#define PACKET_TYPE_CONFIG_UPDATE 0x20

// Standard Header for Mesh Networking
struct PacketHeader {
    uint32_t deviceID;     // Unique Sender ID
    uint8_t  messageID;    // Random uint8 to identify unique packets (Deduplication)
    uint8_t  hopCount;     // TTL (Time To Live), starts at 3
    uint8_t  packetType;   // 0x01=Location, 0x02=Heartbeat, 0x10=Cmd_Report
} __attribute__((packed));

// Payload for Location Data
struct LocationPayload {
    float lat;
    float lon;
    uint8_t battery;       // Battery percentage or voltage encoding
} __attribute__((packed));

// Payload for Acknowledgement
struct AckPayload {
    uint32_t ackDeviceID;  // The deviceID being acknowledged
    uint8_t  ackMessageID; // The messageID being acknowledged
} __attribute__((packed));

// Payload for Configuration (if needed)
struct ConfigPayload {
    uint8_t count;
    uint8_t uuids[5][16]; 
} __attribute__((packed));

// Full Packet Wrapper (Union/Struct for convenience)
// Note: In practice, we might send Header + Payload buffer directly.
// This struct is just for size estimation or static allocation.
struct MeshPacket {
    PacketHeader header;
    union {
        LocationPayload location;
        AckPayload      ack;
        ConfigPayload   config;
        // uint8_t raw[max_payload_size];
    } payload;
} __attribute__((packed));

#endif

