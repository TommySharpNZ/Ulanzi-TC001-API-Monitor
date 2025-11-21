#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// Project Details
String buildNumber = "v1.0.6";

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
const unsigned long batteryUpdateInterval = 10000; // Update battery every 10 seconds
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
  Serial.println("\n\nTC001 Custom Firmware " + buildNumber + " Starting...");
  
  // Initialize WiFi to get proper MAC address
  WiFi.mode(WIFI_STA);
  delay(100); // Short delay to let WiFi initialize
  
  // Get MAC address and create unique device ID
  uint8_t mac[6];
  WiFi.macAddress(mac);
  deviceID = String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
  deviceID.toUpperCase();
  deviceName = "TC001-Display-" + deviceID;
  
  // Set hostname for router identification
  WiFi.setHostname(deviceName.c_str());
  
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
  String batteryText = String(batteryPercentage) + "% ";
  
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
  autoBrightness = preferences.getBool("autoBrightness", false);
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

void saveAPIConfiguration() {
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
  preferences.putBool("autoBrightness", autoBrightness);
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
  
  const int MAX_RETRIES = 2;
  const int TIMEOUT_MS = 10000; // 10 second timeout
  int retryCount = 0;
  bool success = false;
  
  while (retryCount <= MAX_RETRIES && !success) {
    if (retryCount > 0) {
      Serial.println("Retry attempt " + String(retryCount) + " of " + String(MAX_RETRIES));
      delay(1000); // Wait 1 second before retry
    }
    
    HTTPClient http;
    http.begin(apiEndpoint);
    http.setTimeout(TIMEOUT_MS);
    
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
          success = true;
        } else {
          currentValue = "PATH ERROR";
          lastError = "Could not extract value from JSON path";
          Serial.println("Error: " + lastError);
          success = true; // Don't retry for JSON path errors
        }
      } else {
        currentValue = "HTTP " + String(httpCode);
        lastError = "HTTP error: " + String(httpCode);
        Serial.println("HTTP error: " + String(httpCode));
        success = true; // Don't retry for HTTP errors (4xx, 5xx)
      }
    } else {
      // Network/timeout error - these we should retry
      String errorMsg = http.errorToString(httpCode);
      Serial.println("Connection failed: " + errorMsg);
      
      if (retryCount == MAX_RETRIES) {
        // Final attempt failed
        currentValue = "CONN FAIL";
        lastError = errorMsg;
      }
    }
    
    http.end();
    retryCount++;
  }
  
  if (!success) {
    Serial.println("API call failed after " + String(MAX_RETRIES + 1) + " attempts");
  }
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
  server.on("/config/api", handleAPIConfigPage);
  server.on("/config/api/save", HTTP_POST, handleSaveAPIConfig);
  server.on("/config/general", handleGeneralConfig);  // General config page
  server.on("/config/general/save", HTTP_POST, handleSaveGeneralConfig);  // General config save handler
  server.on("/test", handleTestAPI);
  server.on("/reset", handleFactoryReset);
  server.on("/status", handleStatus);
  server.on("/restart", handleRestart);
  server.on("/favicon.ico", handleFavicon);
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
  html += ".button { display: inline-block; background: #4CAF50; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; margin: 5px; border: none; cursor: pointer; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.secondary { background: #008CBA; }";
  html += ".button.danger { background: #f44336; }";
  html += ".status { padding: 15px; border-radius: 5px; margin: 10px 0; }";
  html += ".status.ok { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }";
  html += ".status.error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }";
  html += ".status.warning { background: #fff3cd; color: #856404; border: 1px solid #ffeaa7; }";
  html += "</style>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<h1>TC001-Display-" + deviceID + "</h1>";
  
  // Device Info
  html += "<h2>Device Information</h2>";
  html += "<div class='info-row'><span class='label'>Device ID:</span><span class='value'>" + deviceID + "</span></div>";
  html += "<div class='info-row'><span class='label'>IP Address:</span><span class='value'>" + ipAddress + "</span></div>";
  html += "<div class='info-row'><span class='label'>WiFi SSID:</span><span class='value'>" + String(WiFi.SSID()) + "</span></div>";
  html += "<div class='info-row'><span class='label'>Signal Strength:</span><span class='value'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  html += "<div class='info-row'><span class='label'>Firmware:</span><span class='value'>" + buildNumber + "</span></div>";
  html += "<div class='info-row'><span class='label'>Battery Level:</span><span class='value' id='batteryPercent'>" + String(batteryPercentage) + "%</span></div>";
  html += "<div class='info-row'><span class='label'>Voltage:</span><span class='value' id='batteryVoltage'>" + String(batteryVoltage, 2) + "V</span></div>";  
  
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
  html += "<div class='info-row'><span class='label'>Brightness:</span><span class='value'>" + String(autoBrightness ? "Auto" : "Manual (" + String(manualBrightness) + ")") + "</span></div>";
  
  // Action Buttons
  html += "<h2>Actions</h2>";
  html += "<button onclick='location.href=\"/config/general\"' class='button'>Config</button>";
  html += "<button onclick='location.href=\"/config/api\"' class='button'>Configure API</button>";
  html += "<button onclick='location.href=\"/status\"' class='button secondary'>View JSON Status</button>";
  html += "<button onclick='if(confirm(\"Restart Device?\")) location.href=\"/restart\"' class='button secondary'>Restart</button>";
  html += "<button onclick='if(confirm(\"Reset all settings?\")) location.href=\"/reset\"' class='button danger'>Factory Reset</button>";
  
  // Physical Button Controls Info
  html += "<h2>Button Controls</h2>";
  html += "<div class='info-row'><span class='label'>Button 1 (startup):</span><span class='value'>Enter WiFi config</span></div>";
  html += "<div class='info-row'><span class='label'>Button 2 + 3 (hold):</span><span class='value'>Show battery status</span></div>";
  html += "<div class='info-row'><span class='label'>All 3 (hold 3s):</span><span class='value'>Factory reset</span></div>";
  
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleGeneralConfig() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";  
  html += "<title>General Settings - " + deviceName + "</title>";
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
  html += ".button { display: inline-block; background: #4CAF50; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; margin: 5px; border: none; cursor: pointer; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.secondary { background: #008CBA; }";
  html += ".button.danger { background: #f44336; }";
  html += ".help { font-size: 12px; color: #666; margin-top: 5px; font-style: italic; }";
  html += ".icon-preview { margin-top: 10px; }";
  html += ".icon-pixel { width: 15px; height: 15px; display: inline-block; border: 1px solid #ddd; }";
  html += ".status { padding: 15px; border-radius: 5px; margin: 10px 0; }";
  html += ".status.ok { background: #d4edda; color: #155724; }";
  html += ".status.error { background: #f8d7da; color: #721c24; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>General Settings</h1>";
    
  html += "<form method='POST' action='/config/general/save' accept-charset='utf-8'>";
  
  // Brightness settings section
  html += "<h2>Display Brightness</h2>";

  html += "<label><input type='checkbox' name='autoBrightness' id='autoBrightness' onchange='toggleBrightnessMode()' " + String(autoBrightness ? "checked" : "") + "><span class='checkbox-label'>Auto Brightness</span></label>";
  html += "<p class='help'>Use light sensor for automatic brightness control</p>";
  
  html += "<div id='manualBrightnessGroup' style='display: " + String(autoBrightness ? "none" : "block") + ";'>";
  html += "<label>Manual Brightness:</label>";
  html += "<input type='range' name='brightness' id='brightnessSlider' value='" + String(manualBrightness) + "' min='10' max='255' oninput='updateBrightnessLabel(this.value)'>";
  html += "<span id='brightnessValue'>" + String(manualBrightness) + "</span>";
  html += "<p class='help'>Set brightness level (10-255)</p>";
  html += "</div>";

  html += "<button type='submit' class='button'>Save Settings</button>";
  html += "<button type='button' onclick='location.href=\"/\"' class='button secondary'>Back</button>";
  
  html += "</form>";

  html += "<div id='testResult' style='margin-top: 20px;'></div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function toggleBrightnessMode() {";
  html += "  const isAuto = document.getElementById('autoBrightness').checked;";
  html += "  document.getElementById('manualBrightnessGroup').style.display = isAuto ? 'none' : 'block';";
  html += "}";
  html += "function updateBrightnessLabel(value) {";
  html += "  document.getElementById('brightnessValue').textContent = value;";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveGeneralConfig() {
  Serial.println("=== Saving general configuration ===");
  
  // Debug: Show all received arguments
  Serial.print("Number of arguments received: ");
  Serial.println(server.args());
  for (int i = 0; i < server.args(); i++) {
    Serial.print("  Arg ");
    Serial.print(i);
    Serial.print(": ");
    Serial.print(server.argName(i));
    Serial.print(" = ");
    Serial.println(server.arg(i));
  }
  
  autoBrightness = server.hasArg("autoBrightness");
  Serial.print("Auto Brightness: ");
  Serial.println(autoBrightness ? "ENABLED" : "DISABLED");
  
  if (server.hasArg("brightness")) {
    int newBrightness = server.arg("brightness").toInt();
    Serial.print("Brightness value received: ");
    Serial.println(newBrightness);
    
    manualBrightness = newBrightness;
    if (manualBrightness < 1) manualBrightness = 1;
    if (manualBrightness > 255) manualBrightness = 255;
    
    Serial.print("Brightness value after validation: ");
    Serial.println(manualBrightness);
  } else {
    Serial.println("WARNING: No brightness argument received!");
  }
  
  // Save to preferences
  Serial.println("Writing to preferences...");
  preferences.begin("tc001", false);
  preferences.putBool("autoBrightness", autoBrightness);
  preferences.putInt("brightness", manualBrightness);
  preferences.end();
  Serial.println("Preferences written successfully");
  
  // Apply brightness immediately
  if (!autoBrightness) {
    Serial.print("Applying manual brightness: ");
    Serial.println(manualBrightness);
    matrix.setBrightness(manualBrightness);
  } else {
    Serial.println("Auto brightness enabled - will use light sensor");
  }
  
  Serial.println("=== General settings saved ===");
  
  // Redirect back to general config page
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleAPIConfigPage() {
  String maskedKey = "";
  if (apiKey.length() > 0) {
    for (unsigned int i = 0; i < apiKey.length(); i++) {
      maskedKey += "*";
    }
  }
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>API Settings - " + deviceName + "</title>";  
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
  html += ".button { display: inline-block; background: #4CAF50; color: white; padding: 10px 20px; text-decoration: none; border-radius: 5px; margin: 5px; border: none; cursor: pointer; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.secondary { background: #008CBA; }";
  html += ".button.danger { background: #f44336; }";
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
  
  html += "<form method='POST' action='/config/api/save' accept-charset='utf-8'>";
  
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
  
  html += "<button type='submit' class='button'>Save Configuration</button>";
  html += "<button type='button' class='button secondary' onclick='testAPI()'>Test Connection</button>";
  html += "<button type='button' onclick='location.href=\"/\"' class='button secondary'>Back</button>";
  
  html += "</form>";
  
  html += "<div id='testResult' style='margin-top: 20px;'></div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function testAPI() {";
  html += "  document.getElementById('testResult').innerHTML = '<p>Testing connection...</p>';";
  html += "  fetch('/test').then(r => r.text()).then(data => {";
  html += "    document.getElementById('testResult').innerHTML = '<div class=\"status ok\"><pre>' + data + '</pre></div>';";
  html += "  }).catch(err => {";
  html += "    document.getElementById('testResult').innerHTML = '<div class=\"status error\">Error: ' + err + '</div>';";
  html += "  });";
  html += "}";
  
  html += "let currentColor = [255, 0, 0];";
  html += "let iconDataArray = [];";
  html += "let isUpdatingFromPreview = false;";
  html += "let colorPickerInitialized = false;";
  
  html += "function rgbToHex(r, g, b) {";
  html += "  return '#' + [r, g, b].map(x => {";
  html += "    const hex = x.toString(16);";
  html += "    return hex.length === 1 ? '0' + hex : hex;";
  html += "  }).join('');";
  html += "}";
  
  html += "function hexToRgb(hex) {";
  html += "  const result = /^#?([a-f\\d]{2})([a-f\\d]{2})([a-f\\d]{2})$/i.exec(hex);";
  html += "  return result ? [";
  html += "    parseInt(result[1], 16),";
  html += "    parseInt(result[2], 16),";
  html += "    parseInt(result[3], 16)";
  html += "  ] : [0, 0, 0];";
  html += "}";
  
  html += "function updatePreviewFromJSON() {";
  html += "  if (isUpdatingFromPreview) return;";
  html += "  try {";
  html += "    const data = JSON.parse(document.getElementById('iconData').value);";
  html += "    if (Array.isArray(data) && data.length === 64) {";
  html += "      iconDataArray = data;";
  html += "      renderPreview();";
  html += "    } else {";
  html += "      document.getElementById('iconPreview').innerHTML = '<p style=\"color:red\">Invalid: Need exactly 64 pixels</p>';";
  html += "    }";
  html += "  } catch(e) {";
  html += "    if (document.getElementById('iconData').value.trim() === '') {";
  html += "      iconDataArray = Array(64).fill([0, 0, 0]);";
  html += "      renderPreview();";
  html += "    } else {";
  html += "      document.getElementById('iconPreview').innerHTML = '';";
  html += "    }";
  html += "  }";
  html += "}";
  
  html += "function updateJSONFromPreview() {";
  html += "  isUpdatingFromPreview = true;";
  html += "  document.getElementById('iconData').value = JSON.stringify(iconDataArray);";
  html += "  setTimeout(() => { isUpdatingFromPreview = false; }, 10);";
  html += "}";
  
  html += "function updateColorDisplay() {";
  html += "  const rgbDisplay = document.getElementById('rgbDisplay');";
  html += "  if (rgbDisplay) {";
  html += "    rgbDisplay.textContent = 'RGB: ' + currentColor.join(', ');";
  html += "  }";
  html += "}";
  
  html += "function renderPreview() {";
  html += "  let preview = '<div style=\"margin-bottom: 10px;\">';";
  html += "  preview += '<label>Color Picker: </label>';";
  html += "  preview += '<input type=\"color\" id=\"colorPicker\" value=\"' + rgbToHex(currentColor[0], currentColor[1], currentColor[2]) + '\" style=\"width:60px;height:30px;border:none;cursor:pointer;\">';";
  html += "  preview += '<span id=\"rgbDisplay\" style=\"margin-left:10px;\">RGB: ' + currentColor.join(', ') + '</span>';";
  html += "  preview += '<button type=\"button\" onclick=\"clearIcon()\" style=\"margin-left:15px;padding:5px 10px;cursor:pointer;\">Clear All</button>';";
  html += "  preview += '</div>';";
  html += "  preview += '<div id=\"pixelGrid\" style=\"display:inline-block;border:2px solid #ccc;\">';";
  html += "  for (let i = 0; i < 8; i++) {";
  html += "    for (let j = 0; j < 8; j++) {";
  html += "      const idx = i * 8 + j;";
  html += "      const pixel = iconDataArray[idx] || [0, 0, 0];";
  html += "      const color = 'rgb(' + pixel[0] + ',' + pixel[1] + ',' + pixel[2] + ')';";
  html += "      preview += '<div class=\"icon-pixel\" id=\"pixel' + idx + '\" onclick=\"paintPixel(' + idx + ')\" style=\"background-color:' + color + ';cursor:pointer;\" title=\"Click to paint\"></div>';";
  html += "    }";
  html += "    preview += '<br>';";
  html += "  }";
  html += "  preview += '</div>';";
  html += "  document.getElementById('iconPreview').innerHTML = preview;";
  html += "  if (!colorPickerInitialized) {";
  html += "    setupColorPicker();";
  html += "    colorPickerInitialized = true;";
  html += "  }";
  html += "}";
  
  html += "function setupColorPicker() {";
  html += "  const colorPicker = document.getElementById('colorPicker');";
  html += "  if (colorPicker) {";
  html += "    colorPicker.addEventListener('input', function(e) {";
  html += "      currentColor = hexToRgb(e.target.value);";
  html += "      updateColorDisplay();";
  html += "    });";
  html += "  }";
  html += "}";
  
  html += "function paintPixel(index) {";
  html += "  iconDataArray[index] = [...currentColor];";
  html += "  const pixelElement = document.getElementById('pixel' + index);";
  html += "  if (pixelElement) {";
  html += "    pixelElement.style.backgroundColor = 'rgb(' + currentColor[0] + ',' + currentColor[1] + ',' + currentColor[2] + ')';";
  html += "  }";
  html += "  updateJSONFromPreview();";
  html += "}";
  
  html += "function clearIcon() {";
  html += "  iconDataArray = Array(64).fill([0, 0, 0]);";
  html += "  for (let i = 0; i < 64; i++) {";
  html += "    const pixelElement = document.getElementById('pixel' + i);";
  html += "    if (pixelElement) {";
  html += "      pixelElement.style.backgroundColor = 'rgb(0, 0, 0)';";
  html += "    }";
  html += "  }";
  html += "  updateJSONFromPreview();";
  html += "}";
  
  html += "document.getElementById('iconData').addEventListener('input', updatePreviewFromJSON);";
  
  html += "window.addEventListener('load', function() {";
  html += "  updatePreviewFromJSON();";
  html += "});";
  
  html += "</script>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveAPIConfig() {
  apiEndpoint = server.arg("apiUrl");
  apiHeaderName = server.arg("apiHeader");
  apiKey = server.arg("apiKey");
  jsonPath = server.arg("jsonPath");
  displayPrefix = server.arg("prefix");
  displaySuffix = server.arg("suffix");
  pollingInterval = server.arg("interval").toInt();
  scrollEnabled = server.hasArg("scroll");
  iconData = server.arg("iconData");
  
  if (pollingInterval < 5) pollingInterval = 5;
  if (pollingInterval > 3600) pollingInterval = 3600;
  
  if (iconData.length() > 0) {
    parseIconData(iconData);
  } else {
    iconEnabled = false;
  }
  
  saveAPIConfiguration();
  
  apiConfigured = (apiEndpoint.length() > 0 && jsonPath.length() > 0);
  
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

void handleRestart() {
  Serial.println("Restart requested via web interface");
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Restarting...</title>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f0f0f0;}";
  html += ".container{background:white;padding:30px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);max-width:400px;margin:0 auto;}";
  html += "h1{color:#4CAF50;}</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>Restarting Device</h1>";
  html += "<p>The device is restarting now...</p>";
  html += "<p>Please wait about 20 seconds, then <a href='/'>click here</a> to return to the configuration page.</p>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
  
  delay(1000); // Give time for response to be sent
  ESP.restart();
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

void handleFavicon() {
  // Return a 204 No Content response (tells browser there's no favicon)
  server.send(204);
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
