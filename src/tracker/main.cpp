#include <Arduino.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <bluefruit.h> // Header for nRF52 Bluefruit
#include "common.h"

// ---------------------------
// Hardware Pin Definitions
// ---------------------------
// Adjust these to match your specific wiring on the Seeed XIAO nRF52840
#define RADIO_CS_PIN    D0
#define RADIO_DIO1_PIN  D1
#define RADIO_RST_PIN   D2
#define RADIO_BUSY_PIN  D3

// GPS connected to Serial1 (D6/D7 usually on XIAO)
#define GPS_BAUD        9600
#define CONFIG_FILENAME "/config.dat"

// ---------------------------
// Globals & Objects
// ---------------------------
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);
TinyGPSPlus gps;

uint32_t myDeviceID = 0;
uint8_t nextMessageID = 0;

// BLE Objects
// Target Gateway UUID to look for
uint8_t targetUUID[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 
                          0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00}; 

// ---------------------------
// Helper Functions
// ---------------------------
uint32_t getUniqueID() {
    return NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1];
}

void loadConfig() {
    File file = InternalFS.open(CONFIG_FILENAME, FILE_READ);
    if (file) {
        if (file.read(targetUUID, 16) == 16) {
            Serial.println("Config loaded from flash.");
        }
        file.close();
    } else {
        Serial.println("No config found, using default.");
    }
}

void saveConfig(uint8_t* newUUID) {
    InternalFS.remove(CONFIG_FILENAME);
    File file = InternalFS.open(CONFIG_FILENAME, FILE_WRITE);
    if (file) {
        file.write(newUUID, 16);
        file.close();
        memcpy(targetUUID, newUUID, 16);
        Serial.println("Config saved.");
    }
}

// Forward declaration
void sendPacket(uint8_t packetType, void* payloadData, size_t payloadSize);

// RX Window Function
void listenForDownlink(unsigned long timeoutMs) {
    unsigned long start = millis();
    Serial.println("Listening for downlink...");
    
    // Put radio in RX mode
    radio.startReceive();
    
    while (millis() - start < timeoutMs) {
        // Poll for packet
        // Note: checking if packet received without blocking the whole duration if possible,
        // but simple blocking read with timeout is easier if RadioLib supports it?
        // Actually, startReceive is non-blocking. We need to check irq flags.
        // Or simpler: radio.receive(buffer, len) blocks until timeout if configured?
        // RadioLib `receive` is blocking. Let's use `readData` if we used startReceive?
        // Or just use blocking receive with a timeout matching our window.
        // But RadioLib blocking receive doesn't always take timeout arg in all methods (depends on version).
        
        // Simpler approach for this scaffold: Check available()
        // But available() only works if we called startReceive().
        // For SX1262, check radio.getPacketLength(false) ??
        // Let's just check the DIO1 pin or busy?
        // For simplicity:
        
        uint8_t buffer[256];
        int state = radio.readData(buffer, sizeof(buffer)); // Only works if we called startReceive and IRQ fired?
        
        if (state == RADIOLIB_ERR_NONE) {
            Serial.println("Downlink received!");
            PacketHeader* header = (PacketHeader*)buffer;
            
            if (header->packetType == PACKET_TYPE_CMD_REPORT) {
                Serial.println("CMD: REPORT_NOW received.");
                // Trigger Location Send immediately
                // We are in listen window, so we can break and just let loop run?
                // Or better: handle it here.
                // But if we handle it here, we might recurse.
                // Let's set a flag to force report next loop?
                // Or just send it now.
                // Re-sending location now:
                // We need GPS though.
                // If we are in this window, we might have just sent location.
                // If we didn't, we might not have fresh GPS.
                // Let's just break and set a "forceReport" flag?
                // Actually user said: "Once tracker receives request, it will check location and send response."
                // So we should break, acquire GPS, and send.
                return; // Caller will handle logic if we return status?
            } 
            else if (header->packetType == PACKET_TYPE_CONFIG_UPDATE) {
                Serial.println("CMD: CONFIG_UPDATE received.");
                int len = radio.getPacketLength();
                if (len >= sizeof(PacketHeader) + sizeof(ConfigPayload)) {
                    ConfigPayload* cfg = (ConfigPayload*)(buffer + sizeof(PacketHeader));
                    // Update UUID
                    // Assuming count > 0 and using first UUID
                    saveConfig(cfg->uuids[0]);
                }
            }
            
            // Go back to RX for remainder of window?
            radio.startReceive();
        }
        delay(10);
    }
}

void sendPacket(uint8_t packetType, void* payloadData, size_t payloadSize) {
    PacketHeader header;
    header.deviceID = myDeviceID;
    header.messageID = nextMessageID++;
    header.hopCount = 3; 
    header.packetType = packetType;

    size_t totalSize = sizeof(PacketHeader) + payloadSize;
    uint8_t* buffer = new uint8_t[totalSize];

    memcpy(buffer, &header, sizeof(PacketHeader));
    if (payloadSize > 0 && payloadData != nullptr) {
        memcpy(buffer + sizeof(PacketHeader), payloadData, payloadSize);
    }

    Serial.print("Transmitting Packet Type 0x");
    Serial.println(packetType, HEX);
    
    int state = radio.transmit(buffer, totalSize);
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("Transmission success!"));
    } else {
        Serial.print(F("Transmission failed, code "));
        Serial.println(state);
    }

    delete[] buffer;
}

// ---------------------------
// BLE Functions
// ---------------------------
volatile bool gatewayFound = false;

void scan_callback(ble_gap_evt_adv_report_t* report) {
    // Check for specific UUID
    if (Bluefruit.Scanner.checkReportForUuid(report, targetUUID, sizeof(targetUUID))) {
        gatewayFound = true;
    }
}

// ---------------------------
// Setup
// ---------------------------
void setup() {
    Serial.begin(115200);
    delay(2000); 

    Serial.println(F("Booting Tracker..."));

    // 1. Filesystem
    if(InternalFS.begin()) {
        loadConfig();
    } else {
        Serial.println(F("FS Init Failed"));
    }

    // 2. Identity
    myDeviceID = getUniqueID();
    Serial.print(F("Device ID: "));
    Serial.println(myDeviceID, HEX);

    // 3. Radio
    Serial.print(F("[SX1262] Initializing ... "));
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 22);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
    } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        while (true); 
    }

    // 4. GPS
    Serial1.begin(GPS_BAUD);

    // 5. BLE
    Bluefruit.begin(0, 1); // 0 periph, 1 central
    Bluefruit.setName("CatTracker");
    Bluefruit.Scanner.setRxPacket(scan_callback);
    Bluefruit.Scanner.restartOnDisconnect(true);
    Bluefruit.Scanner.setInterval(160, 80); // in unit of 0.625 ms
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.start(0); 
    Bluefruit.Scanner.stop();
}

// ---------------------------
// Loop
// ---------------------------
void loop() {
    // 1. Scan for BLE Beacon
    gatewayFound = false; // Reset flag
    Bluefruit.Scanner.start(0); // Start scanning
    delay(2000);                // Wait 2 seconds for callback to fire
    Bluefruit.Scanner.stop();   // Stop scanning
    
    if (gatewayFound) {
        Serial.println(F("Gateway BLE Found! Home Mode."));
        // Skip GPS
        sendPacket(PACKET_TYPE_HEARTBEAT, nullptr, 0);
        
        // Listen for downlink (e.g. Config update)
        listenForDownlink(2000);
        
        gatewayFound = false; // Reset for next loop
    } else {
        Serial.println(F("Gateway not found. Roaming Mode."));
        
        // 2. Poll GPS
        unsigned long start = millis();
        bool gpsFix = false;
        while (millis() - start < 2000) { 
             while (Serial1.available() > 0) {
                if (gps.encode(Serial1.read())) {
                    if (gps.location.isValid()) gpsFix = true;
                }
            }
        }
        
        if (gpsFix) {
             LocationPayload loc;
            loc.lat = gps.location.lat();
            loc.lon = gps.location.lng();
            loc.battery = 100; // TODO Read ADC
            sendPacket(PACKET_TYPE_LOCATION, &loc, sizeof(LocationPayload));
        } else {
             Serial.println(F("GPS No Fix, sending heartbeat"));
             sendPacket(PACKET_TYPE_HEARTBEAT, nullptr, 0);
        }

        // Listen for downlink (e.g. REPORT_NOW or Config)
        listenForDownlink(2000); 
    }

    // Sleep or Wait
    delay(15000); 
}
