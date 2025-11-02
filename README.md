# TC001 Custom Firmware - API Display Monitor

Custom firmware for the Ulanzi TC001 pixel display clock that enables portable API monitoring for trade shows and events. This firmware allows the device to poll APIs directly without requiring external servers like AWTRIX.

![TC001 Display](images/ulanzi-tc001.png)

## Table of Contents
- [Overview](#overview)
- [Hardware Specifications](#hardware-specifications)
- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Configuration](#configuration)
- [Usage](#usage)
- [API Requirements](#api-requirements)
- [Button Controls](#button-controls)
- [Development](#development)
- [Future Enhancements](#future-enhancements)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## Overview

The TC001 Custom Firmware transforms your Ulanzi TC001 into a self-contained API monitoring device. Perfect for scenarios where you need portable, at-a-glance monitoring without relying on external infrastructure.

**Key Use Cases:**
- Trade show booth metrics (visitor counts, lead generation)
- Support ticket monitoring on the go
- Real-time sales dashboards
- IoT sensor data display
- Any REST API with JSON responses

**Why This Over AWTRIX?**
AWTRIX requires an external server to POST data to the device. This firmware polls APIs directly from the device, making it ideal for portable use on public WiFi networks where you can't run external servers.

## Hardware Specifications

**Device:** Ulanzi TC001 Pixel Display Clock

**Display:** 32x8 WS2812 LED Matrix

**Microcontroller:** ESP32

### Pinout Reference
| GPIO | Function |
|------|----------|
| GPIO14 | Button 3 |
| GPIO15 | Buzzer |
| GPIO21 | I2C SDA |
| GPIO22 | I2C SCL |
| GPIO26 | Button 1 |
| GPIO27 | Button 2 |
| GPIO32 | WS2812 LED Matrix |
| GPIO34 | ADC Input |
| GPIO35 | Light Sensor |

## Features

### ✅ Current Features

#### WiFi Management
- 🔌 Access Point mode for initial setup
- 🌐 Web-based WiFi configuration
- 💾 Persistent credential storage
- 🔄 Easy reconfiguration via buttons

#### API Integration
- 🔗 Direct API polling (no external server needed)
- 🔐 Custom header authentication support
- ⏱️ Configurable polling intervals (5-3600 seconds)
- 🧭 Flexible JSON path navigation
- 📊 Support for nested objects and arrays

#### Display
- 📜 Continuous scrolling text
- 🎨 Color-coded status (green=ok, red=error, yellow=warning)
- 🏷️ Optional display prefixes
- 📱 Real-time value updates

#### Web Interface
- ⚙️ Full configuration page
- 🧪 API connection testing
- 📊 Status monitoring
- 🔄 Factory reset option

#### Security
- 🔒 Secure NVS storage for API keys
- 🎭 Masked API key display
- 💾 Persistent settings across reboots

#### Device Management
- 🆔 Unique device identification (MAC-based)
- 📡 Unique AP names per device
- 🔴 Hardware and web-based factory reset

## Requirements

### Hardware
- Ulanzi TC001 Pixel Display Clock
- USB-C cable for programming
- Computer with Arduino IDE

### Software
- [Arduino IDE](https://www.arduino.cc/en/software) 1.8.x or 2.x
- [Python](https://www.python.org/downloads/) (for firmware backup via esptool)

### Arduino Libraries
Install via Arduino Library Manager:
1. **WiFiManager** by tzapu
2. **Adafruit GFX Library**
3. **Adafruit NeoMatrix**
4. **Adafruit NeoPixel**
5. **ArduinoJson** by Benoit Blanchon (v6.x or v7.x)

Built-in ESP32 libraries (no installation needed):
- WiFi
- WebServer
- HTTPClient
- Preferences

## Installation

### 1. Backup Stock Firmware (Recommended)

Before flashing custom firmware, backup the original:

```bash
# Install esptool
pip install esptool

# Backup firmware (replace COM3 with your port)
esptool.py --port COM3 --baud 115200 read_flash 0x0 0x400000 tc001_backup.bin
```

To restore stock firmware later:
```bash
esptool.py --port COM3 --baud 115200 write_flash 0x0 tc001_backup.bin
```

### 2. Install Arduino IDE and Libraries

1. Download and install [Arduino IDE](https://www.arduino.cc/en/software)
2. Add ESP32 board support:
   - Go to **File → Preferences**
   - Add to "Additional Board Manager URLs": `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Go to **Tools → Board → Boards Manager**
   - Search for "ESP32" and install

3. Install required libraries:
   - **Sketch → Include Library → Manage Libraries**
   - Search and install each library from the Requirements section

### 3. Upload Firmware

1. Open the `TC001_Custom.ino` sketch in Arduino IDE
2. Configure board settings:
   - **Tools → Board → ESP32 Arduino → ESP32 Dev Module**
   - **Tools → Port →** Select your TC001's COM port
3. Click **Upload** button
4. Wait for compilation and upload to complete

### 4. Initial Setup

1. Device will create WiFi AP: `TC001-XXXXXX`
2. Connect to this AP with your phone/computer
3. Captive portal should open automatically (or navigate to `192.168.4.1`)
4. Enter your WiFi credentials
5. Device will connect and display its IP address
6. Browse to the displayed IP address to configure API settings

## Configuration

### Web Interface Configuration

Navigate to your device's IP address (displayed on the LED matrix) to access the configuration interface.

#### API Settings

| Setting | Description | Example |
|---------|-------------|---------|
| **API Endpoint URL** | Full URL to your API | `https://api.example.com/data` |
| **API Header Name** | Authentication header name | `APIKey`, `Authorization`, `X-API-Key` |
| **API Key** | Your API authentication key | `your-secret-key-here` |
| **JSON Path** | Path to value in JSON response | `TC001MatrixDisplay[0].OpenRequests` |
| **Display Prefix** | Text before the value (optional) | `Tickets: ` |
| **Polling Interval** | Seconds between API calls | `60` (range: 5-3600) |

### JSON Path Examples

The firmware supports flexible JSON path navigation:

**Simple root level:**
```json
{ "count": 42 }
```
JSON Path: `count`

**Nested object:**
```json
{ "data": { "unassigned": 12 } }
```
JSON Path: `data.unassigned`

**Array access:**
```json
{ "results": [{ "value": 99 }] }
```
JSON Path: `results[0].value`

**Complex nested:**
```json
{
  "TC001MatrixDisplay": [
    { "OpenRequests": 12 }
  ]
}
```
JSON Path: `TC001MatrixDisplay[0].OpenRequests`

### Testing API Connection

Before saving, use the **Test Connection** button to verify:
- API endpoint is reachable
- Authentication is working
- JSON path correctly extracts the value
- Response format is as expected

## Usage

### Daily Operation

Once configured, the device will:
1. Auto-connect to saved WiFi on startup
2. Begin polling the API at configured intervals
3. Display the current value continuously (scrolling)
4. Update automatically when new data arrives
5. Show error messages if API fails

### Display States

| Color | Meaning |
|-------|---------|
| 🟢 Green | Normal operation, displaying API value |
| 🔴 Red | Error state (connection failed, parsing error) |
| 🟡 Yellow | Not configured or warning |

### Example Configuration

For a support ticket API:

**API Response:**
```json
{
  "StatusCode": 200,
  "Summary": "OK",
  "RecordCount": 1,
  "TC001MatrixDisplay": [
    {
      "OpenRequests": 12
    }
  ]
}
```

**Configuration:**
- API Endpoint: `https://support.yourcompany.com/api/tickets`
- Header Name: `APIKey`
- API Key: `Bi...5#`
- JSON Path: `TC001MatrixDisplay[0].OpenRequests`
- Display Prefix: `Tickets: `
- Polling Interval: `60`

**Result:** Display shows "Tickets: 12" scrolling continuously, updates every 60 seconds.

## API Requirements

Your API must meet these requirements:

### Required
- ✅ HTTP/HTTPS GET endpoint
- ✅ JSON response format
- ✅ Stable endpoint URL

### Optional
- ⚙️ Header-based authentication (APIKey, Authorization, etc.)
- ⚙️ Support for custom headers

### Response Format
The API can return any valid JSON structure. Use the JSON path configuration to navigate to your desired value. Values can be:
- Numbers: `42`, `3.14`
- Strings: `"active"`, `"12 items"`
- Booleans: `true`, `false`

## Button Controls

### Hardware Buttons

| Action | Result |
|--------|--------|
| **Hold Button 1 during startup** | Enter WiFi configuration mode |
| **Hold all 3 buttons for 3 seconds** | Factory reset (WiFi + API settings) |
| **Hold Left + Right buttons** | Power on/off (stock firmware feature) |

### Factory Reset

Two ways to factory reset:
1. **Hardware:** Hold all 3 buttons for 3 seconds (device beeps twice)
2. **Web Interface:** Click "Factory Reset" button on home page

Factory reset clears:
- ✅ WiFi credentials
- ✅ API configuration
- ✅ All stored settings

Device will restart in AP mode for reconfiguration.

## Development

### Project Structure

Currently single-file Arduino sketch:
```
TC001_Custom/
├── TC001_Custom.ino    # Main firmware file
└── README.md           # This file
```

### Code Organization

Main components:
- **WiFi Management** - WiFiManager integration, AP mode
- **Web Server** - Configuration interface, status pages
- **API Client** - HTTP requests, JSON parsing
- **Display Manager** - LED matrix control, scrolling text
- **Storage** - NVS preferences for configuration
- **Button Handler** - Physical button controls

### Key Functions

```cpp
void pollAPI()                    // Make API request and parse response
String extractJSONValue()         // Navigate JSON path and extract value
void scrollCurrentValue()         // Update LED display
void loadConfiguration()          // Load settings from NVS
void saveConfiguration()          // Save settings to NVS
void handleConfigPage()           // Serve web configuration page
```

### Customization

The firmware can be extended with:
- Multiple API endpoints (button navigation)
- Custom display modes
- Icon support
- Threshold-based color coding
- Data visualization
- Historical trending

### Building From Source

1. Clone the repository
2. Open `TC001_Custom.ino` in Arduino IDE
3. Install required libraries
4. Select ESP32 Dev Module board
5. Compile and upload

## Future Enhancements

Planned features for future releases:

### High Priority
- [ ] Multiple API endpoint support with button navigation
- [ ] 8x8 icon display alongside values
- [ ] HTTPS/SSL support for secure APIs
- [ ] Value formatting options (commas, decimals, units)
- [ ] Brightness adjustment

### Medium Priority
- [ ] Threshold-based color coding
- [ ] OAuth/Bearer token authentication
- [ ] Time-based display modes
- [ ] Data caching for offline operation
- [ ] Display rotation modes

### Low Priority
- [ ] OTA (Over-The-Air) firmware updates
- [ ] Historical data graphing
- [ ] Multi-value display with auto-rotation
- [ ] MQTT support
- [ ] Home Assistant integration

## Troubleshooting

### Device Won't Connect to WiFi

**Symptoms:** Device stuck in AP mode or shows "WIFI FAIL"

**Solutions:**
1. Hold Button 1 during startup to manually enter config mode
2. Verify WiFi credentials are correct
3. Check WiFi signal strength
4. Ensure 2.4GHz network (ESP32 doesn't support 5GHz)

### API Not Updating

**Symptoms:** Display shows old value or error message

**Solutions:**
1. Use "Test Connection" button in web interface
2. Check API endpoint is accessible from device's network
3. Verify API key is correct and not expired
4. Confirm JSON path matches actual response structure
5. Check Serial Monitor for detailed error messages

### Display Shows Wrong Value

**Symptoms:** Extracted value doesn't match expected data

**Solutions:**
1. Verify JSON path syntax in configuration
2. Use "Test Connection" to see raw API response
3. Check for array indices starting at 0
4. Ensure nested object paths use correct notation

### Can't Access Web Interface

**Symptoms:** Can't browse to device's IP address

**Solutions:**
1. Verify device is on same network as your computer
2. Check IP address shown on LED display
3. Try pinging the device: `ping 192.168.x.x`
4. Factory reset and reconfigure if needed

### Serial Monitor Debug

Connect USB and open Serial Monitor (115200 baud) to see:
- Startup messages
- WiFi connection status
- API request/response details
- JSON parsing results
- Error messages

## Technical Notes

### Storage
- WiFi credentials stored in ESP32 WiFi NVS
- API settings stored in custom NVS namespace: `tc001`
- Settings persist across firmware updates
- Factory reset clears both WiFi and custom NVS

### Security Considerations
- API keys stored in plaintext in NVS
- Keys only masked in web UI display
- Consider encryption for highly sensitive deployments
- Use HTTPS endpoints when possible (future feature)

### Network Requirements
- Requires 2.4GHz WiFi (ESP32 limitation)
- Outbound HTTP/HTTPS connectivity needed
- No incoming connections required
- Works on networks with captive portals after initial auth

### Performance
- Memory: ~280KB flash, ~40KB RAM
- API polling: Configurable 5-3600 second intervals
- Display refresh: 50ms per scroll frame
- Web interface: Minimal resource usage

## Contributing

Contributions welcome! Areas for improvement:
- Additional authentication methods
- Display effects and animations
- Multi-API support
- Documentation improvements
- Bug fixes and testing

## License

This project is open source and available under the MIT License.

## Acknowledgments

- **Ulanzi** for the TC001 hardware platform
- **tzapu** for the excellent WiFiManager library
- **Adafruit** for the LED matrix libraries
- **Benoit Blanchon** for ArduinoJson
- The ESP32 Arduino community

## Support

For issues, questions, or feature requests:
- Open an issue on GitHub
- Check existing issues for solutions
- Consult the troubleshooting section above

---

**Device Identification:**
- Device Name Format: `TC001-Display-XXXXXX`
- AP Name Format: `TC001-XXXXXX`
- Where XXXXXX = Last 6 characters of MAC address

**Version:** 1.0.0  
**Last Updated:** 2025-11-03  
**Compatible Hardware:** Ulanzi TC001 Pixel Display Clock
