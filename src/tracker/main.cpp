#include <Arduino.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <bluefruit.h> // Header for nRF52 Bluefruit
#include "common.h"

using namespace Adafruit_LittleFS_Namespace;

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
uint8_t lastSentMessageID = 0; // Track for ACK
bool isGpsAwake = true; // Track GPS power state
int8_t currentTxPower = 22; // Default to max power (22 dBm)
int missedDownlinks = 0; // Track consecutive missed downlinks
int consecutiveGpsFailures = 0; // Track GPS fix failures

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

uint8_t readBatteryLevel() {
#ifdef PIN_VBAT
    // Set resolution
    analogReadResolution(12);
    // Read raw
    int raw = analogRead(PIN_VBAT);
    // Voltage divider (if any) or internal reference? 
    // On XIAO nRF52840, the pin measures battery / 2? No, usually direct or divider.
    // Assuming standard divider: 3.3V reference? 
    // nRF52 ADC reference is usually internal 0.6V with gain 1/6 = 3.6V range.
    // Let's assume raw 4096 = 3.6V (or VDD).
    // Mapping: 4.2V = 100%, 3.3V = 0%.
    // For now, return a simple map or raw for debug.
    
    // Simple mock calculation:
    // float voltage = raw * (3.6 / 4096.0); // Adjust based on actual hardware
    return map(raw, 0, 4096, 0, 100); 
#else
    return 100; // Stub
#endif
}

void loadConfig() {
    File file = InternalFS.open(CONFIG_FILENAME, FILE_O_READ);
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
    File file = InternalFS.open(CONFIG_FILENAME, FILE_O_WRITE);
    if (file) {
        file.write(newUUID, 16);
        file.close();
        memcpy(targetUUID, newUUID, 16);
        Serial.println("Config saved.");
    }
}

// Forward declaration
void sendPacket(uint8_t packetType, void* payloadData, size_t payloadSize);

// GPS Power Management (u-blox / BN-180)
void sleepGPS() {
    if (!isGpsAwake) return;
    // ... (existing code)
    // UBX-RXM-PMREQ to enter Backup Mode (Software Backup)
    uint8_t sleepCmd[] = {0xB5, 0x62, 0x02, 0x41, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4D, 0x3B};
    Serial1.write(sleepCmd, sizeof(sleepCmd));
    isGpsAwake = false;
    Serial.println(F("GPS sleeping..."));
}

void wakeGPS() {
    if (isGpsAwake) return;
    // Send dummy bytes to wake up u-blox from sleep
    Serial1.write(0xFF);
    Serial1.write(0xFF);
    Serial1.flush();
    delay(100); // Allow wake-up time
    isGpsAwake = true;
    Serial.println(F("GPS waking..."));
}

void performLocationUpdate() {
    wakeGPS(); // Wake up GPS for reading

    unsigned long start = millis();
    bool gpsFix = false;
    
    // Dynamic Timeout Logic:
    // Standard timeout: 5 seconds (sufficient for hot start)
    // Cold Start / Trouble timeout: 45 seconds (tried only after multiple failures)
    unsigned long timeout = 5000; 

    if (consecutiveGpsFailures >= 3) {
        Serial.println(F("Multiple GPS failures. Attempting extended Cold Start window..."));
        timeout = 45000; // Give it 45 seconds to find satellites
    }

    // Try to get fix
    while (millis() - start < timeout) { 
            while (Serial1.available() > 0) {
            if (gps.encode(Serial1.read())) {
                if (gps.location.isValid()) {
                    gpsFix = true;
                    // We got a fix! We can break early to save power.
                    // But we might want to wait a bit for HDOP to improve?
                    // For now, let's take the first valid fix to be quick.
                    goto fix_found; 
                }
            }
        }
    }
    
    fix_found:
    if (gpsFix) {
        consecutiveGpsFailures = 0; // Reset counter on success
            LocationPayload loc;
        loc.lat = gps.location.lat();
        loc.lon = gps.location.lng();
        loc.battery = readBatteryLevel();
        sendPacket(PACKET_TYPE_LOCATION, &loc, sizeof(LocationPayload));
    } else {
        consecutiveGpsFailures++;
        Serial.print(F("GPS No Fix. Failure count: "));
        Serial.println(consecutiveGpsFailures);
            sendPacket(PACKET_TYPE_HEARTBEAT, nullptr, 0);
    }

    // Put GPS back to sleep
    sleepGPS();
}

enum RxStatus {
    RX_TIMEOUT,
    RX_ACK,
    RX_CMD_REPORT,
    RX_ERROR
};

// RX Window Function
RxStatus listenForDownlink(unsigned long timeoutMs) {
    unsigned long start = millis();
    Serial.println("Listening for downlink...");
    
    bool receivedAny = false;
    RxStatus result = RX_TIMEOUT;

    // Put radio in RX mode
    radio.startReceive();
    
    while (millis() - start < timeoutMs) {
        uint8_t buffer[256];
        int state = radio.readData(buffer, sizeof(buffer)); 
        
        if (state == RADIOLIB_ERR_NONE) {
            Serial.println("Downlink received!");
            receivedAny = true;
            missedDownlinks = 0; // Reset counter
            PacketHeader* header = (PacketHeader*)buffer;
            int len = radio.getPacketLength(); // Get length of received packet

            if (header->packetType == PACKET_TYPE_ACK) {
                if (len >= sizeof(PacketHeader) + sizeof(AckPayload)) {
                    AckPayload* ack = (AckPayload*)(buffer + sizeof(PacketHeader));
                    if (ack->ackDeviceID == myDeviceID && ack->ackMessageID == lastSentMessageID) {
                        Serial.println("ACK received for last packet!");
                        result = RX_ACK;
                        return RX_ACK; // Return immediately on ACK
                    }
                }
            }
            else if (header->packetType == PACKET_TYPE_CMD_REPORT) {
                Serial.println("CMD: REPORT_NOW received.");
                result = RX_CMD_REPORT;
                return RX_CMD_REPORT; // Return immediately to handle report
            } 
            else if (header->packetType == PACKET_TYPE_CONFIG_UPDATE) {
                Serial.println("CMD: CONFIG_UPDATE received.");
                if (len >= sizeof(PacketHeader) + sizeof(ConfigPayload)) {
                    ConfigPayload* cfg = (ConfigPayload*)(buffer + sizeof(PacketHeader));
                    saveConfig(cfg->uuids[0]);
                }
            }
            
            // Go back to RX for remainder of window (if not returned)
            radio.startReceive();
            
            // Adaptive Power Logic
            if (currentTxPower > 10) {
                 currentTxPower -= 2; 
                 Serial.print(F("Link good. Reducing TX power to "));
                 Serial.println(currentTxPower);
            }
        }
        delay(10);
    }

    if (!receivedAny) {
        missedDownlinks++;
        if (missedDownlinks > 5 && currentTxPower < 22) {
             currentTxPower += 2; 
             Serial.print(F("Missed downlinks. Increasing TX power to "));
             Serial.println(currentTxPower);
             missedDownlinks = 0; 
        }
    }
    
    return result;
}

void sendPacket(uint8_t packetType, void* payloadData, size_t payloadSize) {
    PacketHeader header;
    header.deviceID = myDeviceID;
    header.messageID = nextMessageID++;
    lastSentMessageID = header.messageID;
    header.hopCount = 3; 
    header.packetType = packetType;

    size_t totalSize = sizeof(PacketHeader) + payloadSize;
    uint8_t* buffer = new uint8_t[totalSize];

    memcpy(buffer, &header, sizeof(PacketHeader));
    if (payloadSize > 0 && payloadData != nullptr) {
        memcpy(buffer + sizeof(PacketHeader), payloadData, payloadSize);
    }

    Serial.print(F("Transmitting Packet Type 0x"));
    Serial.println(packetType, HEX);
    
    // Set power before transmitting
    radio.setOutputPower(currentTxPower);

    int state = radio.transmit(buffer, totalSize);
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("Transmission success!"));
        // Adaptive Power Logic:
        // If successful, we might lower power? 
        // No, successful transmit doesn't mean successful reception by gateway.
        // We need an ACK to lower power. 
        // Without ACK, we should probably stay high or increase if we were low.
        // For this simple unacknowledged protocol, we stick to high power or a fixed setting.
        // But user asked for "step up power only if needed".
        // This implies we start low and increase if we fail? 
        // RadioLib `transmit` only fails if hardware fails, not if link fails.
        // We need a downlink (ACK) to know if we reached the gateway.
        
        // Strategy A (Simple): Always use max power for reliability.
        // Strategy B (Adaptive): Start at 10dBm. If no Downlink received in X minutes, step up to 22dBm.
        // Let's implement Strategy B based on 'listenForDownlink' success.
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
    if (Bluefruit.Scanner.checkReportForUuid(report, BLEUuid(targetUUID))) {
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
        radio.setOutputPower(currentTxPower); // Set initial power
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
    Bluefruit.Scanner.setRxCallback(scan_callback);
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
    
    RxStatus rxState = RX_TIMEOUT;

    if (gatewayFound) {
        Serial.println(F("Gateway BLE Found! Home Mode."));
        // Ensure GPS is off to save power
        sleepGPS();

        // Skip GPS
        sendPacket(PACKET_TYPE_HEARTBEAT, nullptr, 0);
        
        // Listen for downlink (e.g. Config update)
        rxState = listenForDownlink(2000);
        
        gatewayFound = false; // Reset for next loop
    } else {
        Serial.println(F("Gateway not found. Roaming Mode."));
        
        performLocationUpdate();

        // Listen for downlink (e.g. REPORT_NOW or Config)
        rxState = listenForDownlink(2000); 
    }

    // Handle CMD_REPORT if received
    if (rxState == RX_CMD_REPORT) {
        Serial.println(F("Executing CMD_REPORT..."));
        performLocationUpdate();
        // Optional: Listen again for ACK?
        // listenForDownlink(1000); 
    }

    // Sleep or Wait
    // Put Radio to sleep to save power
    radio.sleep();
    
    // Low power wait (System ON sleep)
    delay(15000); 
}
