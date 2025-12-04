#include <Arduino.h>
#include <Wire.h>
#include <RadioLib.h>
#include <NimBLEDevice.h>
#include <SSD1306Wire.h>
#include "common.h"

// ---------------------------
// Hardware Pin Definitions
// ---------------------------
// Heltec WiFi LoRa 32 V3 pins
#define LORA_CS     8
#define LORA_DIO1   14
#define LORA_RST    12
#define LORA_BUSY   13

// OLED Pins
#define OLED_SDA    17
#define OLED_SCL    18
#define OLED_RST    21
#define OLED_VEXT   36

// Battery voltage measurement
#define VBAT_ADC_PIN    1    // ADC pin to read battery voltage
#define VBAT_CTRL_PIN   37   // Control pin - must be HIGH to enable voltage divider

// ---------------------------
// Globals & Objects
// ---------------------------
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);
SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

uint32_t myDeviceID = 0;
uint8_t nextMessageID = 0;
uint8_t lastSentMessageID = 0;
int8_t currentTxPower = 22;

// Test GPS coordinates (San Francisco downtown for example)
const float TEST_LAT = 37.7749;
const float TEST_LON = -122.4194;

// ---------------------------
// Helper Functions
// ---------------------------
uint32_t getUniqueID() {
    // ESP32 unique ID from MAC address
    uint64_t mac = ESP.getEfuseMac();
    return (uint32_t)(mac >> 32) ^ (uint32_t)mac;
}

float readBatteryVoltage() {
    // Heltec V3: GPIO 37 HIGH enables divider, GPIO 1 reads voltage
    pinMode(VBAT_CTRL_PIN, OUTPUT);
    digitalWrite(VBAT_CTRL_PIN, HIGH);
    delay(10);
    
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    int raw = analogRead(VBAT_ADC_PIN);
    
    float voltage = (raw / 4095.0) * 3.3 * 4.9;
    
    if (voltage < 2.5) {
        return 0.0;
    }
    
    return voltage;
}

uint8_t readBatteryLevel() {
    // For testing, return random value between 60-100%
    return random(60, 100);
}

void updateDisplay(String status, int packetsSent) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    
    float vbat = readBatteryVoltage();
    String batStr = (vbat > 2.5) ? (String(vbat, 2) + "V") : "USB";
    
    display.drawString(0, 0, "=== Tracker V3 ===");
    display.drawString(0, 12, "Bat: " + batStr);
    display.drawString(0, 24, "ID: " + String(myDeviceID, HEX));
    display.drawString(0, 36, "Pkts: " + String(packetsSent));
    display.drawString(0, 48, status);
    
    display.display();
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
    
    Serial.printf("TX Packet Type 0x%X, MsgID=%d\n", packetType, header.messageID);
    
    int state = radio.transmit(buffer, totalSize);
    
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("TX Success!");
        lastSentMessageID = header.messageID;
    } else {
        Serial.printf("TX Failed, code %d\n", state);
    }
    
    delete[] buffer;
}

// ---------------------------
// Setup
// ---------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Booting Heltec V3 Test Tracker...");

    // Enable OLED power
    pinMode(OLED_VEXT, OUTPUT);
    digitalWrite(OLED_VEXT, LOW);
    delay(10);
    
    // OLED Reset
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);
    delay(100);
    
    // Initialize display
    display.init();
    display.flipScreenVertically();
    display.setContrast(255);
    
    // Brightness boost
    Wire.setClock(700000);
    Wire.beginTransmission(0x3C);
    Wire.write(0x00);
    Wire.write(0xD9);
    Wire.write(0xF1);
    Wire.endTransmission();
    
    // Boot message
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 10, "Heltec V3");
    display.drawString(64, 25, "Test Tracker");
    display.drawString(64, 40, "Initializing...");
    display.display();
    
    delay(2000);

    // Get unique ID
    myDeviceID = getUniqueID();
    Serial.printf("Device ID: 0x%X\n", myDeviceID);

    // Initialize Radio
    Serial.print(F("[SX1262] Initializing ... "));
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 22);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
        radio.setOutputPower(currentTxPower);
    } else {
        Serial.printf("failed, code %d\n", state);
        display.clear();
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 25, "Radio FAILED!");
        display.display();
        while (true) { delay(1000); }
    }

    // Initialize BLE (optional - for beacon scanning)
    NimBLEDevice::init("TestTracker");
    
    Serial.println("Setup complete!");
    updateDisplay("Ready", 0);
}

// ---------------------------
// Loop
// ---------------------------
void loop() {
    static unsigned long lastTx = 0;
    static int packetsSent = 0;
    
    // Send location packet every 30 seconds
    if (millis() - lastTx > 30000) {
        lastTx = millis();
        
        updateDisplay("Sending...", packetsSent);
        
        // Create location payload with test coordinates
        LocationPayload payload;
        payload.lat = TEST_LAT;
        payload.lon = TEST_LON;
        payload.battery = readBatteryLevel();
        
        // Send packet
        sendPacket(PACKET_TYPE_LOCATION, &payload, sizeof(LocationPayload));
        
        packetsSent++;
        
        Serial.printf("Sent location packet #%d: Lat=%.4f, Lon=%.4f, Bat=%d%%\n", 
                      packetsSent, payload.lat, payload.lon, payload.battery);
        
        updateDisplay("TX OK", packetsSent);
    }
    
    delay(100);
}

