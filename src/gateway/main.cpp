#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <RadioLib.h>
#include <NimBLEDevice.h>
#include <LittleFS.h>
#include <SSD1306Wire.h>
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

// OLED Pins
#define OLED_SDA    17
#define OLED_SCL    18
#define OLED_RST    21

// ---------------------------
// Globals & Objects
// ---------------------------
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// Status Globals for Display
String statusLine = "Initializing...";
String lastPacketInfo = "No Data";
String wifiStatusStr = "WiFi: Connecting...";
int lastRssi = 0;

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
#define CACHE_SIZE 64
CacheEntry recentMessages[CACHE_SIZE];
int cacheHead = 0;
uint8_t nextGatewayMessageID = 0; // For gateway generated packets

// Location History for persistent storage
struct LocationHistoryEntry {
    uint32_t deviceID;
    float lat;
    float lon;
    uint8_t battery;
    uint32_t timestamp;  // Unix timestamp (seconds since epoch)
} __attribute__((packed));

#define HISTORY_MAX_ENTRIES 1000  // Maximum number of location entries to store
#define HISTORY_FILENAME "/location_history.dat"
#define HISTORY_MAX_SIZE (HISTORY_MAX_ENTRIES * sizeof(LocationHistoryEntry))

LocationHistoryEntry* locationHistory = nullptr;
uint16_t historyCount = 0;
uint16_t historyHead = 0;  // Circular buffer head pointer
bool historyFull = false;   // True when buffer has wrapped around

// ---------------------------
// Location History Functions
// ---------------------------

// Forward declaration
void saveLocationHistory();

bool initLocationHistory() {
    // Allocate memory for history buffer
    locationHistory = (LocationHistoryEntry*)malloc(HISTORY_MAX_SIZE);
    if (!locationHistory) {
        Serial.println("ERROR: Failed to allocate memory for location history!");
        return false;
    }
    
    // Initialize LittleFS
    if (!LittleFS.begin(true)) {  // true = format if mount fails
        Serial.println("ERROR: LittleFS mount failed!");
        return false;
    }
    
    // Load existing history from flash
    File file = LittleFS.open(HISTORY_FILENAME, "r");
    if (file) {
        size_t fileSize = file.size();
        size_t entriesRead = fileSize / sizeof(LocationHistoryEntry);
        
        if (entriesRead > 0 && entriesRead <= HISTORY_MAX_ENTRIES) {
            // Read all entries sequentially (they're saved in chronological order)
            size_t bytesRead = file.read((uint8_t*)locationHistory, entriesRead * sizeof(LocationHistoryEntry));
            historyCount = bytesRead / sizeof(LocationHistoryEntry);
            
            if (historyCount >= HISTORY_MAX_ENTRIES) {
                // Buffer was full - next write goes to position 0 (oldest)
                historyHead = 0;
                historyFull = true;
            } else {
                // Buffer wasn't full - next write goes after last entry
                historyHead = historyCount;
                historyFull = false;
            }
            
            Serial.printf("Loaded %d location history entries from flash\n", historyCount);
        } else {
            Serial.println("History file size invalid, starting fresh");
            historyCount = 0;
            historyHead = 0;
            historyFull = false;
        }
        file.close();
    } else {
        Serial.println("No existing history file, starting fresh");
        historyCount = 0;
        historyHead = 0;
        historyFull = false;
    }
    
    return true;
}

void addLocationToHistory(uint32_t deviceID, float lat, float lon, uint8_t battery) {
    if (!locationHistory) return;
    
    // Get current time (approximate - ESP32 doesn't have RTC, using millis offset)
    // In production, you'd sync with NTP or GPS time
    uint32_t currentTime = (uint32_t)(millis() / 1000);  // Approximate seconds
    
    // Add entry at head position
    locationHistory[historyHead].deviceID = deviceID;
    locationHistory[historyHead].lat = lat;
    locationHistory[historyHead].lon = lon;
    locationHistory[historyHead].battery = battery;
    locationHistory[historyHead].timestamp = currentTime;
    
    // Update circular buffer
    historyHead = (historyHead + 1) % HISTORY_MAX_ENTRIES;
    
    if (!historyFull) {
        historyCount++;
        if (historyCount >= HISTORY_MAX_ENTRIES) {
            historyFull = true;
        }
    }
    // If historyFull is true, oldest entries are automatically overwritten
    
    // Periodically save to flash (every 10 entries to reduce wear)
    static uint16_t saveCounter = 0;
    saveCounter++;
    if (saveCounter >= 10) {
        saveCounter = 0;
        saveLocationHistory();
    }
}

void saveLocationHistory() {
    if (!locationHistory || historyCount == 0) return;
    
    File file = LittleFS.open(HISTORY_FILENAME, "w");
    if (!file) {
        Serial.println("ERROR: Failed to open history file for writing!");
        return;
    }
    
    if (historyFull) {
        // Buffer has wrapped - historyHead points to oldest entry
        // Write in chronological order: oldest (at historyHead) to newest (at historyHead-1)
        size_t part1Size = (HISTORY_MAX_ENTRIES - historyHead) * sizeof(LocationHistoryEntry);
        size_t part2Size = historyHead * sizeof(LocationHistoryEntry);
        
        // Write part 1: from historyHead (oldest) to end of buffer
        if (part1Size > 0) {
            file.write((uint8_t*)(locationHistory + historyHead), part1Size);
        }
        // Write part 2: from start to historyHead-1 (newest entries)
        if (part2Size > 0) {
            file.write((uint8_t*)locationHistory, part2Size);
        }
    } else {
        // Buffer hasn't wrapped - write sequentially from start (oldest to newest)
        size_t writeSize = historyCount * sizeof(LocationHistoryEntry);
        file.write((uint8_t*)locationHistory, writeSize);
    }
    
    file.close();
    Serial.printf("Saved %d location history entries to flash\n", historyCount);
}

void cleanupOldHistory(uint32_t maxAgeSeconds) {
    if (!locationHistory || historyCount == 0) return;
    
    uint32_t currentTime = (uint32_t)(millis() / 1000);
    uint16_t removed = 0;
    uint16_t newCount = 0;
    
    if (historyFull) {
        // For wrapped buffer, we need to check all entries
        // This is complex, so we'll just save and let natural rotation handle it
        // For now, we'll implement a simpler approach: clear if too old
        bool needsCleanup = false;
        for (uint16_t i = 0; i < HISTORY_MAX_ENTRIES; i++) {
            uint16_t idx = (historyHead + i) % HISTORY_MAX_ENTRIES;
            if (currentTime - locationHistory[idx].timestamp > maxAgeSeconds) {
                needsCleanup = true;
                break;
            }
        }
        
        if (needsCleanup) {
            // Clear all and start fresh (simpler than complex rotation)
            historyCount = 0;
            historyHead = 0;
            historyFull = false;
            Serial.println("History too old, cleared all entries");
            saveLocationHistory();
        }
    } else {
        // Linear buffer - remove old entries from start
        uint16_t validStart = 0;
        for (uint16_t i = 0; i < historyCount; i++) {
            if (currentTime - locationHistory[i].timestamp <= maxAgeSeconds) {
                validStart = i;
                break;
            }
        }
        
        if (validStart > 0) {
            // Shift remaining entries
            for (uint16_t i = validStart; i < historyCount; i++) {
                locationHistory[i - validStart] = locationHistory[i];
            }
            historyCount -= validStart;
            historyHead = historyCount;
            removed = validStart;
            Serial.printf("Removed %d old history entries\n", removed);
            saveLocationHistory();
        }
    }
}

// ---------------------------
// Helper Functions
// ---------------------------

void updateDisplay() {
    display.clear();
    
    // Top Bar: WiFi Status
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, wifiStatusStr);
    
    // Middle: Last Packet
    display.drawString(0, 15, "Last Packet:");
    display.setFont(ArialMT_Plain_16);
    display.drawString(0, 28, lastPacketInfo);
    
    // Bottom: RSSI and Status
    display.setFont(ArialMT_Plain_10);
    String rssiStr = "RSSI: " + String(lastRssi) + " dBm";
    display.drawString(0, 50, rssiStr);
    
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 50, statusLine);
    
    display.display();
}

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
    
    Serial.printf("Broadcasting Packet Type 0x%X\n", packetType);
    radio.transmit(buffer, totalSize);
    delete[] buffer;
    
    // Switch back to RX mode
    radio.startReceive(); 
}

void sendAck(uint32_t targetDeviceID, uint8_t targetMessageID) {
    AckPayload ack;
    ack.ackDeviceID = targetDeviceID;
    ack.ackMessageID = targetMessageID;
    broadcastCommand(PACKET_TYPE_ACK, &ack, sizeof(AckPayload));
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
                 uuidStr.trim(); // Remove any whitespace
                 
                 if (uuidStr.length() >= 32) {
                     ConfigPayload config;
                     config.count = 1;
                     
                     // Parse hex string to uuids[0]
                     bool parseSuccess = true;
                     for (int i = 0; i < 16; i++) {
                         String byteStr = uuidStr.substring(i * 2, i * 2 + 2);
                         char* endPtr;
                         long val = strtol(byteStr.c_str(), &endPtr, 16);
                         if (*endPtr != 0) {
                             Serial.println("Error parsing UUID hex string");
                             parseSuccess = false;
                             break;
                         }
                         config.uuids[0][i] = (uint8_t)val;
                     }
                     
                     if (parseSuccess) {
                         Serial.println("Received CONFIG_UUID command, broadcasting...");
                         broadcastCommand(PACKET_TYPE_CONFIG_UPDATE, &config, sizeof(ConfigPayload));
                     }
                 } else {
                     Serial.println("Invalid UUID string length");
                 }
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
    
    // OLED Reset & Init
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(20);
    digitalWrite(OLED_RST, HIGH);
    
    display.init();
    display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Booting Gateway...");
    display.display();
    
    delay(2000);
    Serial.println("Booting Gateway...");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // 1. Radio Setup
    Serial.print(F("[SX1262] Initializing ... "));
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 22);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        while (true) {
             digitalWrite(LED_BUILTIN, HIGH);
             delay(100);
             digitalWrite(LED_BUILTIN, LOW);
             delay(100);
        }
    }

    // 2. Initialize Location History
    if (!initLocationHistory()) {
        Serial.println("WARNING: Location history initialization failed!");
    }
    
    // 3. Wi-Fi Setup
    wifiStatusStr = "WiFi: Connecting...";
    updateDisplay();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // 4. BLE Beacon Setup
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

    // 5. Radio Interrupt Setup
    radio.setDio1Action(setFlag);

    // Start Radio Receive
    radio.startReceive();
    
    // Cleanup old history entries on startup (older than 7 days = 604800 seconds)
    cleanupOldHistory(604800);
    
    statusLine = "Running";
    updateDisplay();
}

// ---------------------------
// Loop
// ---------------------------
void loop() {
    // Check WiFi Status periodically and reconnect if needed
    static unsigned long lastLedUpdate = 0;
    static unsigned long lastDisplayUpdate = 0;
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    
    // Update WiFi Status String
    if (wifiConnected) {
        wifiStatusStr = "WiFi: " + WiFi.localIP().toString();
        digitalWrite(LED_BUILTIN, LOW);
    } else {
        wifiStatusStr = "WiFi: Disconnected";
        // Blink LED to indicate WiFi issue
        if (millis() - lastLedUpdate > 500) {
             lastLedUpdate = millis();
             digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
    }
    
    // Periodically update display even if no packets (to show WiFi status changes)
    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();
        updateDisplay();
    }

    // Check for LoRa Packet (Non-blocking)
    if (packetReceived) {
        packetReceived = false; // Reset flag
        
        uint8_t buffer[256];
        int state = radio.readData(buffer, sizeof(buffer));

        if (state == RADIOLIB_ERR_NONE) {
            // Packet received
            digitalWrite(LED_BUILTIN, HIGH);
            delay(10);
            digitalWrite(LED_BUILTIN, LOW);

            int len = radio.getPacketLength();
            lastRssi = (int)radio.getRSSI();
            
            if (len < sizeof(PacketHeader)) {
                Serial.println("Packet too short");
                statusLine = "Err: Short Pkt";
            } else {
                PacketHeader* header = (PacketHeader*)buffer;
                lastPacketInfo = String(header->deviceID, HEX);
                statusLine = "Rx: " + String(header->packetType);
                
                Serial.printf("Rx Packet: DevID=%X, MsgID=%d, Hops=%d, Type=%d\n", 
                              header->deviceID, header->messageID, header->hopCount, header->packetType);

                if (!isMessageSeen(header->deviceID, header->messageID)) {
                    addToCache(header->deviceID, header->messageID);

                    // Send ACK for valid packets (Location or Heartbeat)
                    if (header->packetType == PACKET_TYPE_LOCATION || header->packetType == PACKET_TYPE_HEARTBEAT) {
                        // Delay slightly to let tracker switch to RX? 
                        // The tracker switches immediately after TX, so we should be fine.
                        // But we are in "readData" here, which might be slightly after reception.
                        // We should probably send ACK before long upload process?
                        // Or after? If upload fails, do we ACK?
                        // Usually ACK means "RF received", not "Cloud uploaded".
                        // So we ACK here.
                        delay(50); // Small turnaround delay
                        sendAck(header->deviceID, header->messageID);
                    }

                    if (header->packetType == PACKET_TYPE_LOCATION) {
                        if (len >= sizeof(PacketHeader) + sizeof(LocationPayload)) {
                            LocationPayload* payload = (LocationPayload*)(buffer + sizeof(PacketHeader));
                            
                            // Always save to history, regardless of WiFi status
                            addLocationToHistory(header->deviceID, payload->lat, payload->lon, payload->battery);
                            
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
        
        // Update display immediately on packet
        updateDisplay();
        
        // Resume Listening
        radio.startReceive();
    }
}
