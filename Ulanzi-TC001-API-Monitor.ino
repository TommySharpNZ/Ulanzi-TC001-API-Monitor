#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Pin definitions
#define BUTTON_1 26
#define BUTTON_2 27
#define BUTTON_3 14
#define LED_PIN 32
#define BUZZER 15
#define LIGHT_SENSOR 35
#define BATTERY_ADC 34  // Battery voltage monitoring

// Matrix configuration
#define MATRIX_WIDTH 32
#define MATRIX_HEIGHT 8
#define ICON_WIDTH 8
#define TEXT_WIDTH 24

// Battery configuration
#define BATTERY_MIN_VOLTAGE 3.0  // Minimum battery voltage (empty)
#define BATTERY_MAX_VOLTAGE 4.2  // Maximum battery voltage (full)
#define BATTERY_SAMPLES 10       // Number of samples to average
#define BATTERY_LOW_THRESHOLD 20 // Low battery warning at 20%
#define BATTERY_CRITICAL_THRESHOLD 10 // Critical battery at 10%

// Create matrix object
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, LED_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800
);

// Web server on port 80
WebServer server(80);

// Preferences for persistent storage
Preferences preferences;

// Device info
String deviceName = "";
String deviceID = "";
String ipAddress = "";

// API Configuration
String apiEndpoint = "";
String apiKey = "";
String apiHeaderName = "APIKey";
String jsonPath = "";
String displayPrefix = "";
String displaySuffix = "";
int pollingInterval = 60; // seconds
bool scrollEnabled = true; // true = scroll, false = static display

// Brightness configuration
bool autoBrightness = false; // false = manual, true = auto (light sensor)
int manualBrightness = 40; // 0-255, used when autoBrightness is false
unsigned long lastBrightnessUpdate = 0;
const int brightnessUpdateInterval = 100; // Update brightness every 100ms

// Battery monitoring
float batteryVoltage = 0.0;
int batteryPercentage = 0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 5000; // Update battery every 5 seconds
bool showBatteryRequested = false;
unsigned long batteryDisplayTime = 0;
const unsigned long batteryDisplayDuration = 3000; // Show battery for 3 seconds

// Icon configuration
String iconData = ""; // JSON array format: [[r,g,b],[r,g,b],...]
bool iconEnabled = false;
uint16_t iconPixels[64]; // 8x8 icon in RGB565 format

// API State
String currentValue = "---";
String lastError = "";
unsigned long lastAPICall = 0;
bool apiConfigured = false;

// Button states
bool button1Pressed = false;
bool button2Pressed = false;
bool button3Pressed = false;
unsigned long buttonPressTime = 0;
unsigned long button23PressTime = 0; // For Button 2 + 3 combination

// Scrolling text variables
int16_t scrollX = MATRIX_WIDTH;
unsigned long lastScrollUpdate = 0;
const int scrollDelay = 50;

// Config mode flag and message
bool inConfigMode = false;
String configModeMessage = "";
TaskHandle_t displayTaskHandle = NULL;

// Task for continuous display updates during config mode
void displayUpdateTask(void * parameter) {
  while(true) {
    if (inConfigMode) {
      if (millis() - lastScrollUpdate > scrollDelay) {
        scrollCurrentValue();
        lastScrollUpdate = millis();
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // Delay 10ms
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nTC001 Custom Firmware Starting...");
  Serial.println("Version: 1.1.0 - Battery Monitoring Edition");
  
  // Get MAC address and create unique device ID
  uint8_t mac[6];
  WiFi.macAddress(mac);
  deviceID = String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
  deviceID.toUpperCase();
  deviceName = "TC001-Display-" + deviceID;
  
  Serial.print("Device Name: ");
  Serial.println(deviceName);
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  
  // Initialize buttons
  pinMode(BUTTON_1, INPUT_PULLUP);
  pinMode(BUTTON_2, INPUT_PULLUP);
  pinMode(BUTTON_3, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(LIGHT_SENSOR, INPUT); // Light sensor ADC input
  pinMode(BATTERY_ADC, INPUT);  // Battery ADC input
  
  // Initialize LED matrix
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(manualBrightness); // Will be updated after config load
  matrix.setTextColor(matrix.Color(255, 255, 255));
  
  // Show startup message
  displayScrollText(deviceName.c_str(), matrix.Color(0, 255, 0));
  
  // Initial battery reading
  updateBatteryStatus();
  Serial.print("Initial Battery: ");
  Serial.print(batteryVoltage);
  Serial.print("V (");
  Serial.print(batteryPercentage);
  Serial.println("%)");
  
  // Load saved configuration
  loadConfiguration();
  
  // Check if buttons are held for config mode
  checkConfigMode();
  
  // WiFiManager setup
  WiFiManager wifiManager;
  String apName = "TC001-" + deviceID;
  wifiManager.setAPCallback(configModeCallback);
  
  // Set a reasonable timeout (3 minutes)
  wifiManager.setConfigPortalTimeout(180);
  
  // Start autoConnect - this will block, but our task will handle display updates
  if (!wifiManager.autoConnect(apName.c_str())) {
    Serial.println("Failed to connect and hit timeout");
    
    // Stop display task if running
    if (displayTaskHandle != NULL) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = NULL;
    }
    
    displayScrollText("WIFI FAIL", matrix.Color(255, 0, 0));
    delay(3000);
    ESP.restart();
  }
  
  // Connected - stop the display task and clear config mode flag
  if (displayTaskHandle != NULL) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = NULL;
    Serial.println("Display update task deleted");
  }
  inConfigMode = false;
  
  // Connected!
  Serial.println("Connected to WiFi!");
  ipAddress = WiFi.localIP().toString();
  Serial.print("IP Address: ");
  Serial.println(ipAddress);
  
  displayScrollText(ipAddress.c_str(), matrix.Color(0, 255, 0));
  
  // Setup web server routes
  setupWebServer();
  server.begin();
  Serial.println("Web server started");
  
  displayScrollText("READY", matrix.Color(0, 255, 255));
  
  // Poll API immediately if configured
  if (apiConfigured) {
    Serial.println("Performing initial API poll...");
    pollAPI();
    lastAPICall = millis();
  }
  
  // Reset scroll position
  scrollX = MATRIX_WIDTH;
}

void loop() {
  server.handleClient();
  checkButtons();
  
  // Update brightness if in auto mode
  if (autoBrightness && (millis() - lastBrightnessUpdate > brightnessUpdateInterval)) {
    updateBrightness();
    lastBrightnessUpdate = millis();
  }
  
  // Update battery status periodically
  if (millis() - lastBatteryUpdate > batteryUpdateInterval) {
    updateBatteryStatus();
    lastBatteryUpdate = millis();
  }
  
  // Check if battery display time has elapsed
  if (showBatteryRequested && (millis() - batteryDisplayTime > batteryDisplayDuration)) {
    showBatteryRequested = false;
    scrollX = MATRIX_WIDTH; // Reset scroll for normal display
  }
  
  // Poll API if configured
  if (apiConfigured && (millis() - lastAPICall > (pollingInterval * 1000))) {
    pollAPI();
    lastAPICall = millis();
  }
  
  // Continuously scroll the current value or battery info
  if (millis() - lastScrollUpdate > scrollDelay) {
    if (showBatteryRequested) {
      scrollBatteryDisplay();
    } else {
      scrollCurrentValue();
    }
    lastScrollUpdate = millis();
  }
  
  delay(10);
}

// ============================================
// Battery Monitoring Functions
// ============================================

void updateBatteryStatus() {
  // Take multiple samples and average them for more stable readings
  long sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    sum += analogRead(BATTERY_ADC);
    delay(5);
  }
  int rawValue = sum / BATTERY_SAMPLES;
  
  // Convert ADC reading to voltage
  // ESP32 ADC is 12-bit (0-4095) with 3.3V reference
  // Adjust the multiplier based on your voltage divider ratio
  // Common TC001 voltage divider is 2:1, so multiply by 2
  // You may need to calibrate this value for your specific hardware
  batteryVoltage = (rawValue / 4095.0) * 3.3 * 2.0;
  
  // Apply calibration offset if needed (measure actual voltage and adjust)
  // batteryVoltage += 0.1; // Example: add 0.1V if readings are consistently low
  
  // Calculate percentage using voltage curve
  batteryPercentage = calculateBatteryPercentage(batteryVoltage);
  
  // Debug output
  Serial.print("Battery: ");
  Serial.print(batteryVoltage, 2);
  Serial.print("V (");
  Serial.print(batteryPercentage);
  Serial.print("%) - Raw ADC: ");
  Serial.println(rawValue);
  
  // Check for low battery warning
  if (batteryPercentage <= BATTERY_CRITICAL_THRESHOLD && batteryPercentage > 0) {
    // Critical battery - beep twice
    tone(BUZZER, 2000, 100);
    delay(150);
    tone(BUZZER, 2000, 100);
  } else if (batteryPercentage <= BATTERY_LOW_THRESHOLD && batteryPercentage > 0) {
    // Low battery - single beep (only once per boot)
    static bool lowBatteryWarned = false;
    if (!lowBatteryWarned) {
      tone(BUZZER, 1500, 200);
      lowBatteryWarned = true;
    }
  }
}

int calculateBatteryPercentage(float voltage) {
  // LiPo voltage curve (approximate)
  // 4.2V = 100%, 3.7V = 50%, 3.0V = 0%
  
  if (voltage >= BATTERY_MAX_VOLTAGE) {
    return 100;
  } else if (voltage <= BATTERY_MIN_VOLTAGE) {
    return 0;
  }
  
  // Non-linear mapping for more accurate LiPo curve
  // LiPo batteries have a relatively flat discharge curve from 100% to 20%,
  // then drop quickly from 20% to 0%
  
  if (voltage > 3.9) {
    // 100% to 75% range (4.2V to 3.9V)
    return map(voltage * 100, 390, 420, 75, 100);
  } else if (voltage > 3.7) {
    // 75% to 40% range (3.9V to 3.7V)
    return map(voltage * 100, 370, 390, 40, 75);
  } else if (voltage > 3.5) {
    // 40% to 15% range (3.7V to 3.5V)
    return map(voltage * 100, 350, 370, 15, 40);
  } else {
    // 15% to 0% range (3.5V to 3.0V)
    return map(voltage * 100, 300, 350, 0, 15);
  }
}

void scrollBatteryDisplay() {
  String batteryText = "BAT: " + String(batteryPercentage) + "% ";
  batteryText += String(batteryVoltage, 2) + "V";
  
  // Choose color based on battery level
  uint16_t color;
  if (batteryPercentage <= BATTERY_CRITICAL_THRESHOLD) {
    color = matrix.Color(255, 0, 0); // Red for critical
  } else if (batteryPercentage <= BATTERY_LOW_THRESHOLD) {
    color = matrix.Color(255, 165, 0); // Orange for low
  } else {
    color = matrix.Color(0, 255, 0); // Green for good
  }
  
  matrix.fillScreen(0);
  matrix.setTextColor(color);
  
  if (scrollEnabled) {
    // Scrolling mode
    int16_t textWidth = batteryText.length() * 6;
    
    matrix.setCursor(scrollX, 0);
    matrix.print(batteryText);
    matrix.show();
    
    scrollX--;
    if (scrollX < -textWidth) {
      scrollX = MATRIX_WIDTH;
    }
  } else {
    // Static mode - center the text
    int16_t x1, y1;
    uint16_t w, h;
    matrix.getTextBounds(batteryText.c_str(), 0, 0, &x1, &y1, &w, &h);
    int16_t centerX = (MATRIX_WIDTH - w) / 2;
    
    matrix.setCursor(centerX, 0);
    matrix.print(batteryText);
    matrix.show();
  }
}

void showBatteryOnDisplay() {
  showBatteryRequested = true;
  batteryDisplayTime = millis();
  scrollX = MATRIX_WIDTH; // Reset scroll position
  updateBatteryStatus(); // Get latest reading
  Serial.println("Battery display requested via button press");
}

// ============================================
// Original Functions (with battery integration)
// ============================================

void loadConfiguration() {
  preferences.begin("tc001", false);
  
  apiEndpoint = preferences.getString("apiUrl", "");
  apiKey = preferences.getString("apiKey", "");
  apiHeaderName = preferences.getString("apiHeader", "APIKey");
  jsonPath = preferences.getString("jsonPath", "");
  displayPrefix = preferences.getString("prefix", "");
  displaySuffix = preferences.getString("suffix", "");
  pollingInterval = preferences.getInt("interval", 60);
  scrollEnabled = preferences.getBool("scroll", true);
  iconData = preferences.getString("iconData", "");
  autoBrightness = preferences.getBool("autoBright", false);
  manualBrightness = preferences.getInt("brightness", 40);
  
  preferences.end();
  
  apiConfigured = (apiEndpoint.length() > 0 && jsonPath.length() > 0);
  
  // Apply brightness setting
  if (autoBrightness) {
    updateBrightness(); // Read sensor and set brightness
  } else {
    matrix.setBrightness(manualBrightness);
  }
  
  // Parse icon data if present
  if (iconData.length() > 0) {
    parseIconData(iconData);
  }
  
  if (apiConfigured) {
    Serial.println("API Configuration loaded:");
    Serial.println("  Endpoint: " + apiEndpoint);
    Serial.println("  Header: " + apiHeaderName);
    Serial.println("  JSON Path: " + jsonPath);
    Serial.println("  Prefix: " + displayPrefix);
    Serial.println("  Suffix: " + displaySuffix);
    Serial.println("  Interval: " + String(pollingInterval) + "s");
    Serial.println("  Scroll: " + String(scrollEnabled ? "Yes" : "No"));
    Serial.println("  Auto Brightness: " + String(autoBrightness ? "Yes" : "No"));
    if (!autoBrightness) {
      Serial.println("  Manual Brightness: " + String(manualBrightness));
    }
    Serial.println("  Icon: " + String(iconEnabled ? "Enabled" : "Disabled"));
  } else {
    Serial.println("API not configured yet");
  }
}

void saveConfiguration() {
  preferences.begin("tc001", false);
  
  preferences.putString("apiUrl", apiEndpoint);
  preferences.putString("apiKey", apiKey);
  preferences.putString("apiHeader", apiHeaderName);
  preferences.putString("jsonPath", jsonPath);
  preferences.putString("prefix", displayPrefix);
  preferences.putString("suffix", displaySuffix);
  preferences.putInt("interval", pollingInterval);
  preferences.putBool("scroll", scrollEnabled);
  preferences.putString("iconData", iconData);
  preferences.putBool("autoBright", autoBrightness);
  preferences.putInt("brightness", manualBrightness);
  
  preferences.end();
  
  Serial.println("Configuration saved");
}

void checkButtons() {
  // Read all button states
  bool btn1 = (digitalRead(BUTTON_1) == LOW);
  bool btn2 = (digitalRead(BUTTON_2) == LOW);
  bool btn3 = (digitalRead(BUTTON_3) == LOW);
  
  // Check for Button 2 + Button 3 combination to show battery
  if (btn2 && btn3) {
    if (!button2Pressed || !button3Pressed) {
      // Just pressed
      button23PressTime = millis();
      button2Pressed = true;
      button3Pressed = true;
    } else if (millis() - button23PressTime > 500) {
      // Held for 500ms - show battery
      showBatteryOnDisplay();
      // Wait for release
      while (digitalRead(BUTTON_2) == LOW || digitalRead(BUTTON_3) == LOW) {
        delay(50);
      }
      button2Pressed = false;
      button3Pressed = false;
    }
    return; // Don't check other button combinations
  } else {
    button2Pressed = false;
    button3Pressed = false;
  }
  
  // Check for factory reset (all 3 buttons)
  if (btn1 && btn2 && btn3) {
    if (!button1Pressed) {
      buttonPressTime = millis();
      button1Pressed = true;
    }
    
    if (millis() - buttonPressTime > 3000) {
      performFactoryReset();
      button1Pressed = false;
    }
  } else {
    button1Pressed = false;
  }
}

void performFactoryReset() {
  Serial.println("Factory reset initiated!");
  
  // Beep twice to confirm
  tone(BUZZER, 2000, 200);
  delay(300);
  tone(BUZZER, 2000, 200);
  delay(300);
  
  displayScrollText("FACTORY RESET", matrix.Color(255, 0, 0));
  
  preferences.begin("tc001", false);
  preferences.clear();
  preferences.end();
  
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  
  delay(1000);
  ESP.restart();
}

void checkConfigMode() {
  if (digitalRead(BUTTON_1) == LOW) {
    Serial.println("Button 1 held - entering config mode");
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    delay(500);
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  inConfigMode = true;
  configModeMessage = "CONFIG: " + String(myWiFiManager->getConfigPortalSSID());
  
  // Create task for continuous display updates
  xTaskCreatePinnedToCore(
    displayUpdateTask,
    "DisplayTask",
    4096,
    NULL,
    1,
    &displayTaskHandle,
    0
  );
  
  Serial.println("Display update task created");
  currentValue = configModeMessage;
  scrollX = MATRIX_WIDTH;
}

void pollAPI() {
  if (!apiConfigured) {
    return;
  }
  
  HTTPClient http;
  http.begin(apiEndpoint);
  
  if (apiKey.length() > 0) {
    http.addHeader(apiHeaderName, apiKey);
  }
  
  Serial.println("Polling API: " + apiEndpoint);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("API Response received (" + String(payload.length()) + " bytes)");
      
      String value = extractJSONValue(payload, jsonPath);
      
      if (value.length() > 0) {
        currentValue = displayPrefix + value + displaySuffix;
        lastError = "";
        Serial.println("Extracted value: " + value);
        Serial.println("Display value: " + currentValue);
        scrollX = MATRIX_WIDTH;
      } else {
        currentValue = "PATH ERROR";
        lastError = "Could not extract value from JSON path";
        Serial.println("Error: " + lastError);
      }
    } else {
      currentValue = "HTTP " + String(httpCode);
      lastError = "HTTP error: " + String(httpCode);
      Serial.println("HTTP error: " + String(httpCode));
    }
  } else {
    currentValue = "CONN FAIL";
    lastError = http.errorToString(httpCode);
    Serial.println("Connection failed: " + lastError);
  }
  
  http.end();
}

String extractJSONValue(const String& json, const String& path) {
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, json);
  
  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    return "";
  }
  
  JsonVariant current = doc.as<JsonVariant>();
  String workingPath = path;
  
  while (workingPath.length() > 0) {
    int dotPos = workingPath.indexOf('.');
    int bracketPos = workingPath.indexOf('[');
    
    String segment;
    if (bracketPos >= 0 && (dotPos < 0 || bracketPos < dotPos)) {
      segment = workingPath.substring(0, bracketPos);
      if (segment.length() > 0 && current.is<JsonObject>()) {
        current = current[segment];
      }
      
      int closeBracket = workingPath.indexOf(']');
      if (closeBracket < 0) {
        Serial.println("Malformed path: missing ]");
        return "";
      }
      
      String arrayPart = workingPath.substring(bracketPos + 1, closeBracket);
      
      if (arrayPart.indexOf('=') > 0) {
        int eqPos = arrayPart.indexOf('=');
        String filterField = arrayPart.substring(0, eqPos);
        String filterValue = arrayPart.substring(eqPos + 1);
        
        if (current.is<JsonArray>()) {
          JsonArray arr = current.as<JsonArray>();
          bool found = false;
          
          for (JsonVariant item : arr) {
            if (item.is<JsonObject>()) {
              JsonVariant fieldValue = item[filterField];
              if (fieldValue.is<const char*>()) {
                if (String(fieldValue.as<const char*>()) == filterValue) {
                  current = item;
                  found = true;
                  break;
                }
              } else if (fieldValue.is<int>()) {
                if (String(fieldValue.as<int>()) == filterValue) {
                  current = item;
                  found = true;
                  break;
                }
              }
            }
          }
          
          if (!found) {
            Serial.println("No matching item found in array");
            return "";
          }
        }
      } else {
        int index = arrayPart.toInt();
        if (current.is<JsonArray>()) {
          JsonArray arr = current.as<JsonArray>();
          if (index >= 0 && index < arr.size()) {
            current = arr[index];
          } else {
            Serial.println("Array index out of bounds");
            return "";
          }
        }
      }
      
      workingPath = workingPath.substring(closeBracket + 1);
      if (workingPath.startsWith(".")) {
        workingPath = workingPath.substring(1);
      }
    } else if (dotPos >= 0) {
      segment = workingPath.substring(0, dotPos);
      workingPath = workingPath.substring(dotPos + 1);
      
      if (current.is<JsonObject>()) {
        current = current[segment];
      } else {
        Serial.println("Expected object at segment: " + segment);
        return "";
      }
    } else {
      segment = workingPath;
      workingPath = "";
      
      if (current.is<JsonObject>()) {
        current = current[segment];
      }
    }
  }
  
  if (current.is<const char*>()) {
    return String(current.as<const char*>());
  } else if (current.is<int>()) {
    return String(current.as<int>());
  } else if (current.is<float>()) {
    return String(current.as<float>(), 2);
  } else if (current.is<bool>()) {
    return current.as<bool>() ? "true" : "false";
  }
  
  Serial.println("Value is not a primitive type");
  return "";
}

void scrollCurrentValue() {
  matrix.fillScreen(0);
  
  uint16_t color;
  if (lastError.length() > 0) {
    color = matrix.Color(255, 0, 0);
  } else {
    color = matrix.Color(0, 255, 0);
  }
  matrix.setTextColor(color);
  
  if (scrollEnabled) {
    int iconOffset = iconEnabled ? (ICON_WIDTH + 1) : 0;
    int16_t textWidth = currentValue.length() * 6;
    
    if (iconEnabled && scrollX < ICON_WIDTH) {
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < ICON_WIDTH; x++) {
          if (scrollX + x >= 0 && scrollX + x < MATRIX_WIDTH) {
            matrix.drawPixel(scrollX + x, y, iconPixels[y * 8 + x]);
          }
        }
      }
    }
    
    matrix.setCursor(scrollX + iconOffset, 0);
    matrix.print(currentValue);
    matrix.show();
    
    scrollX--;
    if (scrollX < -(textWidth + iconOffset)) {
      scrollX = MATRIX_WIDTH;
    }
  } else {
    int displayWidth = iconEnabled ? TEXT_WIDTH : MATRIX_WIDTH;
    int xOffset = iconEnabled ? ICON_WIDTH : 0;
    
    if (iconEnabled) {
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
          matrix.drawPixel(x, y, iconPixels[y * 8 + x]);
        }
      }
    }
    
    int16_t x1, y1;
    uint16_t w, h;
    matrix.getTextBounds(currentValue.c_str(), 0, 0, &x1, &y1, &w, &h);
    
    int16_t centerX = xOffset + (displayWidth - w) / 2;
    if (centerX < xOffset) centerX = xOffset;
    
    matrix.setCursor(centerX, 0);
    matrix.print(currentValue);
    matrix.show();
  }
}

void parseIconData(const String& jsonData) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, jsonData);
  
  if (error) {
    Serial.print("Icon parse error: ");
    Serial.println(error.c_str());
    iconEnabled = false;
    return;
  }
  
  if (!doc.is<JsonArray>()) {
    Serial.println("Icon data is not an array");
    iconEnabled = false;
    return;
  }
  
  JsonArray pixels = doc.as<JsonArray>();
  if (pixels.size() != 64) {
    Serial.print("Icon must have 64 pixels, got: ");
    Serial.println(pixels.size());
    iconEnabled = false;
    return;
  }
  
  for (int i = 0; i < 64; i++) {
    JsonArray pixel = pixels[i];
    if (!pixel || pixel.size() < 3) {
      Serial.print("Invalid pixel at index: ");
      Serial.println(i);
      iconEnabled = false;
      return;
    }
    
    uint8_t r = pixel[0];
    uint8_t g = pixel[1];
    uint8_t b = pixel[2];
    
    iconPixels[i] = matrix.Color(r, g, b);
  }
  
  iconEnabled = true;
  Serial.println("Icon parsed successfully (64 pixels)");
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/config", handleConfigPage);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/test", handleTestAPI);
  server.on("/reset", handleFactoryReset);
  server.on("/status", handleStatus);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; border-bottom: 2px solid #4CAF50; padding-bottom: 10px; }";
  html += "h2 { color: #555; margin-top: 20px; }";
  html += ".info-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #eee; }";
  html += ".label { font-weight: bold; color: #666; }";
  html += ".value { color: #333; }";
  html += ".button { display: inline-block; background: #4CAF50; color: white; padding: 10px 20px; ";
  html += "text-decoration: none; border-radius: 5px; margin: 5px; border: none; cursor: pointer; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.secondary { background: #008CBA; }";
  html += ".button.danger { background: #f44336; }";
  html += ".status { padding: 15px; border-radius: 5px; margin: 10px 0; }";
  html += ".status.ok { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }";
  html += ".status.error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }";
  html += ".status.warning { background: #fff3cd; color: #856404; border: 1px solid #ffeaa7; }";
  
  // Battery status styles
  html += ".battery-container { display: flex; align-items: center; gap: 10px; margin: 15px 0; }";
  html += ".battery-bar { width: 200px; height: 30px; border: 2px solid #333; border-radius: 5px; position: relative; overflow: hidden; }";
  html += ".battery-fill { height: 100%; transition: width 0.3s ease; }";
  html += ".battery-fill.good { background: #4CAF50; }";
  html += ".battery-fill.low { background: #FFA726; }";
  html += ".battery-fill.critical { background: #f44336; }";
  html += ".battery-text { position: absolute; width: 100%; text-align: center; line-height: 30px; font-weight: bold; color: #333; }";
  html += ".battery-tip { width: 6px; height: 18px; background: #333; border-radius: 0 3px 3px 0; margin-left: 2px; }";
  
  html += "</style>";
  html += "<script>";
  html += "function updateBattery() {";
  html += "  fetch('/status').then(r => r.json()).then(data => {";
  html += "    document.getElementById('batteryPercent').textContent = data.battery_percentage + '%';";
  html += "    document.getElementById('batteryVoltage').textContent = data.battery_voltage + 'V';";
  html += "    const fill = document.getElementById('batteryFill');";
  html += "    fill.style.width = data.battery_percentage + '%';";
  html += "    fill.className = 'battery-fill ' + (data.battery_percentage > 20 ? 'good' : data.battery_percentage > 10 ? 'low' : 'critical');";
  html += "    document.getElementById('batteryText').textContent = data.battery_percentage + '%';";
  html += "  });";
  html += "}";
  html += "setInterval(updateBattery, 5000);";
  html += "window.addEventListener('load', updateBattery);";
  html += "</script>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<h1>TC001-Display-" + deviceID + "</h1>";
  
  // Battery Status Section
  html += "<h2>Battery Status</h2>";
  html += "<div class='battery-container'>";
  html += "<div class='battery-bar'>";
  html += "<div id='batteryFill' class='battery-fill good' style='width: " + String(batteryPercentage) + "%'></div>";
  html += "<div id='batteryText' class='battery-text'>" + String(batteryPercentage) + "%</div>";
  html += "</div>";
  html += "<div class='battery-tip'></div>";
  html += "</div>";
  html += "<div class='info-row'>";
  html += "<span class='label'>Battery Level:</span>";
  html += "<span class='value' id='batteryPercent'>" + String(batteryPercentage) + "%</span>";
  html += "</div>";
  html += "<div class='info-row'>";
  html += "<span class='label'>Voltage:</span>";
  html += "<span class='value' id='batteryVoltage'>" + String(batteryVoltage, 2) + "V</span>";
  html += "</div>";
  
  // Device Info
  html += "<h2>Device Information</h2>";
  html += "<div class='info-row'><span class='label'>Device ID:</span><span class='value'>" + deviceID + "</span></div>";
  html += "<div class='info-row'><span class='label'>IP Address:</span><span class='value'>" + ipAddress + "</span></div>";
  html += "<div class='info-row'><span class='label'>WiFi SSID:</span><span class='value'>" + String(WiFi.SSID()) + "</span></div>";
  html += "<div class='info-row'><span class='label'>Signal Strength:</span><span class='value'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  html += "<div class='info-row'><span class='label'>Firmware:</span><span class='value'>v1.1.0 (Battery)</span></div>";
  
  // API Status
  html += "<h2>API Status</h2>";
  if (apiConfigured) {
    html += "<div class='status ok'>";
    html += "<strong>Configured</strong><br>";
    html += "Current Value: <strong>" + currentValue + "</strong><br>";
    html += "Last Update: " + String((millis() - lastAPICall) / 1000) + "s ago";
    if (lastError.length() > 0) {
      html += "<br>Last Error: " + lastError;
    }
    html += "</div>";
  } else {
    html += "<div class='status warning'>";
    html += "<strong>Not Configured</strong><br>";
    html += "Please configure API settings to begin monitoring.";
    html += "</div>";
  }
  
  // Display Settings
  html += "<h2>Display Settings</h2>";
  html += "<div class='info-row'><span class='label'>Scroll Mode:</span><span class='value'>" + String(scrollEnabled ? "Enabled" : "Static") + "</span></div>";
  html += "<div class='info-row'><span class='label'>Brightness:</span><span class='value'>" + String(autoBrightness ? "Auto" : "Manual (" + String(manualBrightness) + ")") + "</span></div>";
  html += "<div class='info-row'><span class='label'>Icon:</span><span class='value'>" + String(iconEnabled ? "Enabled" : "Disabled") + "</span></div>";
  
  // Action Buttons
  html += "<h2>Actions</h2>";
  html += "<a href='/config' class='button'>Configure API</a>";
  html += "<a href='/status' class='button secondary'>View JSON Status</a>";
  html += "<button onclick='if(confirm(\"Reset all settings?\")) location.href=\"/reset\"' class='button danger'>Factory Reset</button>";
  
  // Button Controls Info
  html += "<h2>Button Controls</h2>";
  html += "<div class='info-row'><span class='label'>Button 1 (startup):</span><span class='value'>Enter WiFi config</span></div>";
  html += "<div class='info-row'><span class='label'>Button 2 + 3 (hold):</span><span class='value'>Show battery status</span></div>";
  html += "<div class='info-row'><span class='label'>All 3 (hold 3s):</span><span class='value'>Factory reset</span></div>";
  
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfigPage() {
  String maskedKey = "";
  if (apiKey.length() > 0) {
    for (unsigned int i = 0; i < apiKey.length(); i++) {
      maskedKey += "*";
    }
  }
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; border-bottom: 2px solid #4CAF50; padding-bottom: 10px; }";
  html += "label { display: block; margin-top: 15px; font-weight: bold; color: #555; }";
  html += "input[type='text'], input[type='password'], input[type='number'], textarea { ";
  html += "width: 100%; padding: 10px; margin-top: 5px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }";
  html += "textarea { min-height: 60px; font-family: monospace; }";
  html += "input[type='checkbox'] { margin-top: 10px; }";
  html += ".checkbox-label { display: inline-block; margin-left: 8px; font-weight: normal; }";
  html += ".button { background: #4CAF50; color: white; padding: 12px 24px; border: none; ";
  html += "border-radius: 5px; cursor: pointer; font-size: 16px; margin: 10px 5px 0 0; text-decoration: none; display: inline-block; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.secondary { background: #008CBA; }";
  html += ".help { font-size: 12px; color: #666; margin-top: 5px; font-style: italic; }";
  html += ".icon-preview { margin-top: 10px; }";
  html += ".icon-pixel { width: 15px; height: 15px; display: inline-block; border: 1px solid #ddd; }";
  html += ".status { padding: 15px; border-radius: 5px; margin: 10px 0; }";
  html += ".status.ok { background: #d4edda; color: #155724; }";
  html += ".status.error { background: #f8d7da; color: #721c24; }";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<h1>API Configuration</h1>";
  
  html += "<form method='POST' action='/save'>";
  
  html += "<label>API Endpoint URL:</label>";
  html += "<input type='text' name='apiUrl' value='" + apiEndpoint + "' placeholder='https://api.example.com/data' required>";
  html += "<p class='help'>Full URL to your API endpoint</p>";
  
  html += "<label>API Header Name:</label>";
  html += "<input type='text' name='apiHeader' value='" + apiHeaderName + "' placeholder='APIKey'>";
  html += "<p class='help'>Authentication header name (e.g., APIKey, Authorization, X-API-Key)</p>";
  
  html += "<label>API Key:</label>";
  html += "<input type='password' name='apiKey' value='" + apiKey + "' placeholder='" + (apiKey.length() > 0 ? maskedKey : "your-api-key-here") + "'>";
  html += "<p class='help'>Your API authentication key</p>";
  
  html += "<label>JSON Path:</label>";
  html += "<input type='text' name='jsonPath' value='" + jsonPath + "' placeholder='data.count' required>";
  html += "<p class='help'>Path to value in JSON response (e.g., data.count or users[0].name or items[status=active].count)</p>";
  
  html += "<label>Display Prefix:</label>";
  html += "<input type='text' name='prefix' value='" + displayPrefix + "' placeholder='Tickets: '>";
  html += "<p class='help'>Text to show before the value (optional)</p>";
  
  html += "<label>Display Suffix:</label>";
  html += "<input type='text' name='suffix' value='" + displaySuffix + "' placeholder=' open'>";
  html += "<p class='help'>Text to show after the value (optional)</p>";
  
  html += "<label>Icon Data (8x8 RGB pixels):</label>";
  html += "<textarea name='iconData' id='iconData' placeholder='[[255,0,0],[0,255,0],...]'>" + iconData + "</textarea>";
  html += "<p class='help'>JSON array of 64 RGB pixels [r,g,b] for 8x8 icon (optional)</p>";
  html += "<div id='iconPreview' class='icon-preview'></div>";
  
  html += "<label><input type='checkbox' name='scroll' " + String(scrollEnabled ? "checked" : "") + "><span class='checkbox-label'>Enable Scrolling</span></label>";
  html += "<p class='help'>Uncheck for static centered display</p>";
  
  html += "<label>Polling Interval (seconds):</label>";
  html += "<input type='number' name='interval' value='" + String(pollingInterval) + "' min='5' max='3600' required>";
  html += "<p class='help'>How often to poll the API (5-3600 seconds)</p>";
  
  html += "<label><input type='checkbox' name='autoBright' id='autoBright' onchange='toggleBrightnessMode()' " + String(autoBrightness ? "checked" : "") + "><span class='checkbox-label'>Auto Brightness</span></label>";
  html += "<p class='help'>Use light sensor for automatic brightness control</p>";
  
  html += "<div id='manualBrightnessGroup' style='display: " + String(autoBrightness ? "none" : "block") + ";'>";
  html += "<label>Manual Brightness:</label>";
  html += "<input type='range' name='brightness' id='brightnessSlider' value='" + String(manualBrightness) + "' min='10' max='255' oninput='updateBrightnessLabel(this.value)'>";
  html += "<span id='brightnessValue'>" + String(manualBrightness) + "</span>";
  html += "<p class='help'>Set brightness level (10-255)</p>";
  html += "</div>";
  
  html += "<button type='submit' class='button'>Save Configuration</button>";
  html += "<button type='button' class='button secondary' onclick='testAPI()'>Test Connection</button>";
  html += "<a href='/' class='button secondary'>Cancel</a>";
  
  html += "</form>";
  
  html += "<div id='testResult' style='margin-top: 20px;'></div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function toggleBrightnessMode() {";
  html += "  const isAuto = document.getElementById('autoBright').checked;";
  html += "  document.getElementById('manualBrightnessGroup').style.display = isAuto ? 'none' : 'block';";
  html += "}";
  html += "function updateBrightnessLabel(value) {";
  html += "  document.getElementById('brightnessValue').textContent = value;";
  html += "}";
  html += "function testAPI() {";
  html += "  document.getElementById('testResult').innerHTML = '<p>Testing connection...</p>';";
  html += "  fetch('/test').then(r => r.text()).then(data => {";
  html += "    document.getElementById('testResult').innerHTML = '<div class=\"status ok\"><pre>' + data + '</pre></div>';";
  html += "  }).catch(err => {";
  html += "    document.getElementById('testResult').innerHTML = '<div class=\"status error\">Error: ' + err + '</div>';";
  html += "  });";
  html += "}";
  
  html += "document.getElementById('iconData').addEventListener('input', function() {";
  html += "  try {";
  html += "    const data = JSON.parse(this.value);";
  html += "    if (Array.isArray(data) && data.length === 64) {";
  html += "      let preview = '<div>';";
  html += "      for (let i = 0; i < 8; i++) {";
  html += "        for (let j = 0; j < 8; j++) {";
  html += "          const pixel = data[i * 8 + j];";
  html += "          if (Array.isArray(pixel) && pixel.length >= 3) {";
  html += "            const color = 'rgb(' + pixel[0] + ',' + pixel[1] + ',' + pixel[2] + ')';";
  html += "            preview += '<div class=\"icon-pixel\" style=\"background-color:' + color + '\"></div>';";
  html += "          }";
  html += "        }";
  html += "        preview += '<br>';";
  html += "      }";
  html += "      preview += '</div>';";
  html += "      document.getElementById('iconPreview').innerHTML = preview;";
  html += "    } else {";
  html += "      document.getElementById('iconPreview').innerHTML = '<p style=\"color:red\">Invalid: Need exactly 64 pixels</p>';";
  html += "    }";
  html += "  } catch(e) {";
  html += "    document.getElementById('iconPreview').innerHTML = '';";
  html += "  }";
  html += "});";
  
  html += "window.addEventListener('load', function() {";
  html += "  document.getElementById('iconData').dispatchEvent(new Event('input'));";
  html += "});";
  
  html += "</script>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveConfig() {
  apiEndpoint = server.arg("apiUrl");
  apiHeaderName = server.arg("apiHeader");
  apiKey = server.arg("apiKey");
  jsonPath = server.arg("jsonPath");
  displayPrefix = server.arg("prefix");
  displaySuffix = server.arg("suffix");
  pollingInterval = server.arg("interval").toInt();
  scrollEnabled = server.hasArg("scroll");
  iconData = server.arg("iconData");
  autoBrightness = server.hasArg("autoBright");
  manualBrightness = server.arg("brightness").toInt();
  
  if (pollingInterval < 5) pollingInterval = 5;
  if (pollingInterval > 3600) pollingInterval = 3600;
  if (manualBrightness < 10) manualBrightness = 10;
  if (manualBrightness > 255) manualBrightness = 255;
  
  if (iconData.length() > 0) {
    parseIconData(iconData);
  } else {
    iconEnabled = false;
  }
  
  saveConfiguration();
  
  apiConfigured = (apiEndpoint.length() > 0 && jsonPath.length() > 0);
  
  if (autoBrightness) {
    updateBrightness();
  } else {
    matrix.setBrightness(manualBrightness);
  }
  
  if (apiConfigured) {
    pollAPI();
    lastAPICall = millis();
  }
  scrollX = MATRIX_WIDTH;
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta http-equiv='refresh' content='2;url=/'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; text-align: center; padding-top: 50px; }";
  html += ".message { background: white; padding: 30px; border-radius: 10px; display: inline-block; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='message'>";
  html += "<h1>Configuration Saved!</h1>";
  html += "<p>Redirecting to home page...</p>";
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleTestAPI() {
  if (apiEndpoint.length() == 0) {
    server.send(400, "text/plain", "Error: No API endpoint configured");
    return;
  }
  
  HTTPClient http;
  http.begin(apiEndpoint);
  
  if (apiKey.length() > 0) {
    http.addHeader(apiHeaderName, apiKey);
  }
  
  int httpCode = http.GET();
  String response = "HTTP Code: " + String(httpCode) + "\n\n";
  
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      response += "Response:\n" + payload + "\n\n";
      
      String extractedValue = extractJSONValue(payload, jsonPath);
      if (extractedValue.length() > 0) {
        response += "Extracted Value: " + extractedValue;
      } else {
        response += "Error: Could not extract value from JSON path: " + jsonPath;
      }
    } else {
      response += "Error: " + http.getString();
    }
  } else {
    response += "Error: " + http.errorToString(httpCode);
  }
  
  http.end();
  
  server.send(200, "text/plain", response);
}

void handleFactoryReset() {
  server.send(200, "text/plain", "Resetting...");
  delay(1000);
  
  displayScrollText("FACTORY RESET", matrix.Color(255, 0, 0));
  
  preferences.begin("tc001", false);
  preferences.clear();
  preferences.end();
  
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  
  delay(1000);
  ESP.restart();
}

void handleStatus() {
  String json = "{";
  json += "\"device\":\"" + deviceName + "\",";
  json += "\"device_id\":\"" + deviceID + "\",";
  json += "\"ip\":\"" + ipAddress + "\",";
  json += "\"ssid\":\"" + String(WiFi.SSID()) + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"api_configured\":" + String(apiConfigured ? "true" : "false") + ",";
  json += "\"icon_enabled\":" + String(iconEnabled ? "true" : "false") + ",";
  json += "\"current_value\":\"" + currentValue + "\",";
  json += "\"last_error\":\"" + lastError + "\",";
  json += "\"battery_voltage\":" + String(batteryVoltage, 2) + ",";
  json += "\"battery_percentage\":" + String(batteryPercentage);
  json += "}";
  
  server.send(200, "application/json", json);
}

void displayScrollText(const char* text, uint16_t color) {
  matrix.fillScreen(0);
  matrix.setTextColor(color);
  
  int16_t textWidth = strlen(text) * 6;
  
  for (int x = MATRIX_WIDTH; x > -textWidth; x--) {
    matrix.fillScreen(0);
    matrix.setCursor(x, 0);
    matrix.print(text);
    matrix.show();
    delay(50);
  }
}

void updateBrightness() {
  int sensorValue = analogRead(LIGHT_SENSOR);
  
  int brightness;
  if (sensorValue < 1000) {
    brightness = map(sensorValue, 0, 1000, 1, 5);
  } else if (sensorValue < 2000) {
    brightness = map(sensorValue, 1000, 2000, 5, 30);
  } else if (sensorValue < 3000) {
    brightness = map(sensorValue, 2000, 3000, 30, 120);
  } else {
    brightness = map(sensorValue, 3000, 4095, 120, 255);
  }
  
  brightness = constrain(brightness, 1, 255);
  matrix.setBrightness(brightness);
}
