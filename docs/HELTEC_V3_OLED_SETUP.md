# Heltec WiFi LoRa 32 V3 OLED Display Setup Guide

## Problem Summary

The Heltec V3's OLED display was showing content but appeared extremely dim compared to the stock firmware, despite maximizing contrast and brightness settings in software.

## Root Cause

The **Vext power pin (GPIO 36)** was not being enabled. Without this, the OLED module only received parasitic power from the I2C lines, resulting in drastically reduced brightness.

## Solution

**Enable GPIO 36 (Vext) BEFORE initializing the display:**

```cpp
// GPIO 36 controls OLED module power
// LOW = Power ON, HIGH = Power OFF
pinMode(OLED_VEXT, OUTPUT);
digitalWrite(OLED_VEXT, LOW);  // Enable power
delay(10);  // Wait for power to stabilize
```

## Complete Initialization Sequence

```cpp
#define OLED_SDA    17
#define OLED_SCL    18
#define OLED_RST    21
#define OLED_VEXT   36  // Power control pin

void setup() {
    // 1. ENABLE POWER FIRST
    pinMode(OLED_VEXT, OUTPUT);
    digitalWrite(OLED_VEXT, LOW);  // LOW = ON
    delay(10);
    
    // 2. Reset the display
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);
    delay(100);
    
    // 3. Initialize display
    display.init();
    display.flipScreenVertically();
    display.setContrast(255);
    
    // 4. (Optional) Additional brightness boost
    Wire.setClock(700000);  // Increase I2C speed
    
    Wire.beginTransmission(0x3C);
    Wire.write(0x00);  // Command mode
    Wire.write(0xD9);  // Pre-charge command
    Wire.write(0xF1);  // Max pre-charge
    Wire.endTransmission();
    
    // 5. Display content
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Hello World!");
    display.display();
}
```

## Pin Configuration

| Signal | GPIO | Description |
|--------|------|-------------|
| SDA    | 17   | I²C data line |
| SCL    | 18   | I²C clock line |
| RST    | 21   | Display reset pin |
| **Vext** | **36** | **Power control (critical!)** |

## Key Points

1. **Always enable Vext FIRST** before any display operations
2. GPIO 36 must be driven **LOW** to power the display
3. Allow 10ms for power stabilization before reset
4. The display will technically work without Vext but will be extremely dim
5. The stock Heltec firmware always enables Vext - this is why it looks bright

## Display Specifications

- **Type**: 0.96" 128×64 OLED
- **Controller**: SSD1306
- **Color**: Blue (some variants may be white or dual-color)
- **I²C Address**: 0x3C

## Libraries

### Recommended: SSD1306Wire (ThingPulse)
```ini
[env:gateway]
lib_deps = 
    thingpulse/ESP8266 and ESP32 OLED driver for SSD1306 displays @ ^4.6.1
```

### Alternative: U8g2
```ini
[env:gateway]
lib_deps = 
    olikraus/U8g2 @ ^2.35.0
```

Both work well once Vext is properly enabled.

## Troubleshooting

### Display is blank
- Verify Vext (GPIO 36) is set to OUTPUT and driven LOW
- Check I²C connections (SDA=17, SCL=18)
- Ensure display.display() is called after drawing

### Display is dim
- Confirm GPIO 36 is LOW (use multimeter to verify voltage)
- Increase contrast: `display.setContrast(255)`
- Check power supply can provide sufficient current

### Meshtastic-specific issue
Per [Heltec documentation](https://heltec.org/project/wifi-lora-32-v3/#Specifications):
> Change display setting to SSD1306 instead of auto-detect:
> "Radio configuration" → "Display" → "Override OLED auto-detect" → "SSD1306"

## References

- [Heltec V3 Official Page](https://heltec.org/project/wifi-lora-32-v3/)
- [Reference Implementation](https://github.com/Jufraka/Display_OLED_PLATFORMIO)
- [Heltec Docs](https://docs.heltec.org/)

## Credits

Solution discovered through analysis of reference firmware and community examples showing proper Vext power management.

---

**Last Updated**: December 2024  
**Board**: Heltec WiFi LoRa 32 V3 (ESP32-S3)  
**Display**: 128×64 SSD1306 OLED

