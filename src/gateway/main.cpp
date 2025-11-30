#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <RadioLib.h>
#include <NimBLEDevice.h>
#include "common.h"
#include "secrets.h"

// ---------------------------
// Hardware Pin Definitions
// ---------------------------
// Heltec WiFi LoRa 32 V3 Specific Pins
#define LORA_CS     8
#define LORA_DIO1   14
#define LORA_RST    12
#define LORA_BUSY   13

// ---------------------------
// Globals & Objects
// ---------------------------
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Interrupt Flag
volatile bool packetReceived = false;

void setFlag() {
    packetReceived = true;
}

// Message Cache to prevent broadcast storms
struct CacheEntry {
    uint32_t deviceID;
    uint8_t messageID;
    unsigned long timestamp;
};
#define CACHE_SIZE 20
CacheEntry recentMessages[CACHE_SIZE];
int cacheHead = 0;
uint8_t nextGatewayMessageID = 0; // For gateway generated packets

// ---------------------------
// Helper Functions
// ---------------------------

// Check if message has been seen recently
bool isMessageSeen(uint32_t deviceID, uint8_t messageID) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (recentMessages[i].deviceID == deviceID && recentMessages[i].messageID == messageID) {
            // Check expiry? (e.g. 1 minute) - Optional, simple dedupe for now
            return true;
        }
    }
    return false;
}

void addToCache(uint32_t deviceID, uint8_t messageID) {
    recentMessages[cacheHead].deviceID = deviceID;
    recentMessages[cacheHead].messageID = messageID;
    recentMessages[cacheHead].timestamp = millis();
    cacheHead = (cacheHead + 1) % CACHE_SIZE;
}

void broadcastCommand(uint8_t packetType, void* payload, size_t size) {
    PacketHeader header;
    header.deviceID = 0; // Gateway ID (0 or specific)
    header.messageID = nextGatewayMessageID++;
    header.hopCount = 3;
    header.packetType = packetType;

    size_t totalSize = sizeof(PacketHeader) + size;
    uint8_t* buffer = new uint8_t[totalSize];
    
    memcpy(buffer, &header, sizeof(PacketHeader));
    if (size > 0 && payload != nullptr) {
        memcpy(buffer + sizeof(PacketHeader), payload, size);
    }
    
    Serial.printf("Broadcasting Command Type 0x%X\n", packetType);
    radio.transmit(buffer, totalSize);
    delete[] buffer;
    
    // Switch back to RX mode
    radio.startReceive(); 
}

void uploadToTraccar(PacketHeader* header, LocationPayload* payload) {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    String url = String(TRACCAR_URL) + "/?id=" + String(header->deviceID) 
               + "&lat=" + String(payload->lat, 6) 
               + "&lon=" + String(payload->lon, 6)
               + "&batt=" + String(payload->battery);
    
    Serial.print("Uploading to Traccar: ");
    Serial.println(url);

    http.begin(url);
    int httpCode = http.GET(); // Traccar often uses GET for OsmAnd protocol
    if (httpCode > 0) {
        Serial.printf("HTTP Response code: %d\n", httpCode);
        // Check for commands in response (e.g. REPORT_NOW)
        String response = http.getString();
        
        if (response.indexOf("REPORT_NOW") >= 0) {
            Serial.println("Received REPORT_NOW command!");
            broadcastCommand(PACKET_TYPE_CMD_REPORT, nullptr, 0);
        } else if (response.indexOf("CONFIG_UUID") >= 0) {
             // Expecting "CONFIG_UUID:112233445566778899AABBCCDDEEFF00" (Hex string)
             int idx = response.indexOf("CONFIG_UUID:");
             if (idx >= 0) {
                 String uuidStr = response.substring(idx + 12);
                 // Simple parsing of 32 hex chars (16 bytes)
                 ConfigPayload config;
                 config.count = 1;
                 // Parse hex string to uuids[0]
                 // ... Implementation details for hex parsing ...
                 // For now, assuming fixed format and broadcasting dummy or parsed
                 Serial.println("Received CONFIG_UUID command, broadcasting...");
                 broadcastCommand(PACKET_TYPE_CONFIG_UPDATE, &config, sizeof(ConfigPayload));
             }
        }
    } else {
        Serial.printf("HTTP Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}

void retransmitPacket(uint8_t* data, size_t len) {
    PacketHeader* header = (PacketHeader*)data;
    
    // Decrement TTL
    if (header->hopCount > 0) {
        header->hopCount--;
        
        // Random Delay (Collision Avoidance)
        delay(random(50, 200));

        // Retransmit
        Serial.printf("Retransmitting packet from Device %X, Hops left: %d\n", header->deviceID, header->hopCount);
        radio.transmit(data, len);
        radio.startReceive(); // Back to RX
    } else {
        Serial.println("Packet TTL expired, dropping.");
    }
}

// ---------------------------
// Setup
// ---------------------------
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("Booting Gateway...");

    // 1. Radio Setup
    Serial.print(F("[SX1262] Initializing ... "));
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 22);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        while (true);
    }

    // 2. Wi-Fi Setup
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // 3. BLE Beacon Setup
    NimBLEDevice::init("GatewayBeacon");
    NimBLEServer* pServer = NimBLEDevice::createServer();
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    
    // Create a custom service UUID for beacon
    NimBLEService* pService = pServer->createService(BEACON_UUID);
    pService->start();
    
    pAdvertising->addServiceUUID(BEACON_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->start();
    Serial.println("BLE Beacon Advertising Started");

    // 4. Radio Interrupt Setup
    radio.setDio1Action(setFlag);

    // Start Radio Receive
    radio.startReceive();
}

// ---------------------------
// Loop
// ---------------------------
void loop() {
    // Check WiFi Status periodically and reconnect if needed
    if (WiFi.status() != WL_CONNECTED) {
        // Serial.println("WiFi Disconnected"); 
    }

    // Check for LoRa Packet (Non-blocking)
    if (packetReceived) {
        packetReceived = false; // Reset flag
        
        uint8_t buffer[256];
        int state = radio.readData(buffer, sizeof(buffer));

        if (state == RADIOLIB_ERR_NONE) {
            // Packet received
            int len = radio.getPacketLength();
            if (len < sizeof(PacketHeader)) {
                Serial.println("Packet too short");
            } else {
                PacketHeader* header = (PacketHeader*)buffer;
                Serial.printf("Rx Packet: DevID=%X, MsgID=%d, Hops=%d, Type=%d\n", 
                              header->deviceID, header->messageID, header->hopCount, header->packetType);

                if (!isMessageSeen(header->deviceID, header->messageID)) {
                    addToCache(header->deviceID, header->messageID);

                    if (header->packetType == PACKET_TYPE_LOCATION) {
                        if (len >= sizeof(PacketHeader) + sizeof(LocationPayload)) {
                            LocationPayload* payload = (LocationPayload*)(buffer + sizeof(PacketHeader));
                            if (WiFi.status() == WL_CONNECTED) {
                                uploadToTraccar(header, payload);
                            } else {
                                retransmitPacket(buffer, len);
                            }
                        }
                    } else {
                        // Forward others
                        if (WiFi.status() != WL_CONNECTED) {
                            retransmitPacket(buffer, len);
                        }
                    }
                } else {
                    Serial.println("Duplicate packet, ignoring.");
                }
            }
        }
        
        // Resume Listening
        radio.startReceive();
    }
}
