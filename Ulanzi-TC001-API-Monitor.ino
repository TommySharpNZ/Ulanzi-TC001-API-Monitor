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

// Matrix configuration
#define MATRIX_WIDTH 32
#define MATRIX_HEIGHT 8
#define ICON_WIDTH 8
#define TEXT_WIDTH 24

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
unsigned long buttonPressTime = 0;

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
  
  // Initialize LED matrix
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(40);
  matrix.setTextColor(matrix.Color(255, 255, 255));
  
  // Show startup message
  displayScrollText(deviceName.c_str(), matrix.Color(0, 255, 0));
  
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
  
  // Reset scroll position
  scrollX = MATRIX_WIDTH;
}

void loop() {
  server.handleClient();
  checkButtons();
  
  // Poll API if configured
  if (apiConfigured && (millis() - lastAPICall > (pollingInterval * 1000))) {
    pollAPI();
    lastAPICall = millis();
  }
  
  // Continuously scroll the current value
  if (millis() - lastScrollUpdate > scrollDelay) {
    scrollCurrentValue();
    lastScrollUpdate = millis();
  }
  
  delay(10);
}

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
  
  preferences.end();
  
  apiConfigured = (apiEndpoint.length() > 0 && jsonPath.length() > 0);
  
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
    Serial.println("  Scroll: " + String(scrollEnabled ? "Enabled" : "Disabled"));
    Serial.println("  Icon: " + String(iconEnabled ? "Enabled" : "Disabled"));
  } else {
    Serial.println("No API configuration found");
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
  
  preferences.end();
  
  apiConfigured = (apiEndpoint.length() > 0 && jsonPath.length() > 0);
  Serial.println("Configuration saved");
}

void parseIconData(String data) {
  if (data.length() == 0) {
    iconEnabled = false;
    return;
  }
  
  // Parse JSON array: [[r,g,b],[r,g,b],...]
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, data);
  
  if (error) {
    Serial.print("Icon JSON parsing failed: ");
    Serial.println(error.c_str());
    iconEnabled = false;
    return;
  }
  
  JsonArray array = doc.as<JsonArray>();
  int pixelCount = 0;
  
  for (JsonArray pixel : array) {
    if (pixelCount >= 64) break; // 8x8 = 64 pixels
    
    if (pixel.size() >= 3) {
      uint8_t r = pixel[0];
      uint8_t g = pixel[1];
      uint8_t b = pixel[2];
      iconPixels[pixelCount] = matrix.Color(r, g, b);
      pixelCount++;
    }
  }
  
  iconEnabled = (pixelCount == 64);
  
  if (iconEnabled) {
    Serial.println("Icon parsed successfully: 64 pixels");
  } else {
    Serial.printf("Icon parsing incomplete: %d pixels (need 64)\n", pixelCount);
  }
}

void pollAPI() {
  if (!apiConfigured) return;
  
  Serial.println("Polling API: " + apiEndpoint);
  
  HTTPClient http;
  http.begin(apiEndpoint);
  
  // Add API key header if configured
  if (apiKey.length() > 0) {
    http.addHeader(apiHeaderName, apiKey);
  }
  
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    Serial.printf("HTTP Response: %d\n", httpCode);
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Response: " + payload);
      
      // Parse JSON and extract value
      String extractedValue = extractJSONValue(payload, jsonPath);
      
      if (extractedValue.length() > 0) {
        currentValue = extractedValue;
        lastError = "";
        Serial.println("Extracted value: " + currentValue);
      } else {
        lastError = "JSON path not found";
        Serial.println("Error: " + lastError);
      }
    } else {
      lastError = "HTTP " + String(httpCode);
      Serial.println("Error: " + lastError);
    }
  } else {
    lastError = "Connection failed";
    Serial.printf("Error: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
}

String extractJSONValue(String json, String path) {
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    return "";
  }
  
  // Navigate the JSON path
  // Format: "key1.key2[0].key3" or "key1[Username=Binai Prasad].key3"
  JsonVariant value = doc.as<JsonVariant>();
  
  int start = 0;
  int end = 0;
  
  while (end < path.length()) {
    // Find next separator
    end = path.indexOf('.', start);
    if (end == -1) end = path.length();
    
    String segment = path.substring(start, end);
    
    // Check if segment has array index or filter
    int bracketStart = segment.indexOf('[');
    if (bracketStart != -1) {
      int bracketEnd = segment.indexOf(']');
      String key = segment.substring(0, bracketStart);
      String bracketContent = segment.substring(bracketStart + 1, bracketEnd);
      
      // Check if it's a filter (contains '=') or just an index
      int equalsPos = bracketContent.indexOf('=');
      
      if (equalsPos != -1) {
        // Array filter: key[fieldName=value]
        String filterField = bracketContent.substring(0, equalsPos);
        String filterValue = bracketContent.substring(equalsPos + 1);
        
        // Navigate to the array
        if (key.length() > 0) {
          value = value[key];
        }
        
        // Search through array for matching item
        if (value.is<JsonArray>()) {
          JsonArray arr = value.as<JsonArray>();
          bool found = false;
          
          for (JsonVariant item : arr) {
            if (item[filterField].as<String>() == filterValue) {
              value = item;
              found = true;
              break;
            }
          }
          
          if (!found) {
            Serial.println("No matching item found for filter: " + filterField + "=" + filterValue);
            return "";
          }
        } else {
          Serial.println("Expected array for filtering but got different type");
          return "";
        }
      } else {
        // Regular array index: key[0]
        int index = bracketContent.toInt();
        
        if (key.length() > 0) {
          value = value[key][index];
        } else {
          value = value[index];
        }
      }
    } else {
      value = value[segment];
    }
    
    if (value.isNull()) {
      Serial.println("Path segment not found: " + segment);
      return "";
    }
    
    start = end + 1;
  }
  
  // Convert value to string
  if (value.is<int>()) {
    return String(value.as<int>());
  } else if (value.is<float>()) {
    return String(value.as<float>());
  } else if (value.is<const char*>()) {
    return String(value.as<const char*>());
  } else if (value.is<bool>()) {
    return value.as<bool>() ? "true" : "false";
  }
  
  return "";
}

void scrollCurrentValue() {
  matrix.fillScreen(0);
  
  String displayText;
  uint16_t color;
  
  // Check if in config mode first
  if (inConfigMode) {
    displayText = configModeMessage;
    color = matrix.Color(255, 255, 0); // Yellow for config mode
  } else if (lastError.length() > 0) {
    displayText = lastError;
    color = matrix.Color(255, 0, 0); // Red for errors
  } else if (!apiConfigured) {
    displayText = "NOT CONFIGURED";
    color = matrix.Color(255, 255, 0); // Yellow for not configured
  } else {
    displayText = displayPrefix + currentValue + displaySuffix;
    color = matrix.Color(0, 255, 0); // Green for normal
  }
  
  if (scrollEnabled) {
    // Scrolling mode
    // Draw icon at scroll position if enabled
    if (iconEnabled && !inConfigMode) {
      // Draw icon at current scroll position
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
          int pixelIndex = y * 8 + x;
          int drawX = scrollX + x;
          // Only draw if within visible area
          if (drawX >= 0 && drawX < MATRIX_WIDTH) {
            matrix.drawPixel(drawX, y, iconPixels[pixelIndex]);
          }
        }
      }
    }
    
    // Calculate text start position (after icon if enabled, with 1 pixel gap)
    int textStartX = iconEnabled ? (scrollX + ICON_WIDTH + 1) : scrollX;
    
    matrix.setTextColor(color);
    matrix.setCursor(textStartX, 0);  // y=0 for proper text rendering with descenders
    matrix.print(displayText);
    matrix.show();
    
    scrollX--;
    
    // Calculate total width: icon + gap + text (default font is 6 pixels per character)
    int16_t textWidth = displayText.length() * 6;
    int16_t totalWidth = iconEnabled ? (ICON_WIDTH + 1 + textWidth) : textWidth;
    
    // Reset scroll when content has fully passed
    if (scrollX < -totalWidth) {
      scrollX = MATRIX_WIDTH;
    }
  } else {
    // Static mode - center the content
    int16_t textWidth = displayText.length() * 6;
    int16_t totalWidth = iconEnabled ? (ICON_WIDTH + 1 + textWidth) : textWidth;
    
    // Center the content
    int startX = (MATRIX_WIDTH - totalWidth) / 2;
    if (startX < 0) startX = 0; // If too wide, left-align
    
    // Draw icon if enabled
    if (iconEnabled && !inConfigMode) {
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
          int pixelIndex = y * 8 + x;
          int drawX = startX + x;
          if (drawX >= 0 && drawX < MATRIX_WIDTH) {
            matrix.drawPixel(drawX, y, iconPixels[pixelIndex]);
          }
        }
      }
    }
    
    // Draw text
    int textStartX = iconEnabled ? (startX + ICON_WIDTH + 1) : startX;
    matrix.setTextColor(color);
    matrix.setCursor(textStartX, 0);
    matrix.print(displayText);
    matrix.show();
  }
}

void checkConfigMode() {
  if (digitalRead(BUTTON_1) == LOW) {
    Serial.println("Button 1 held - entering config mode");
    displayScrollText("CONFIG MODE", matrix.Color(255, 255, 0));
    delay(1000);
    
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    wifiManager.setAPCallback(configModeCallback);
    
    String apName = "TC001-" + deviceID;
    wifiManager.startConfigPortal(apName.c_str());
    
    // If portal exits, stop the display task
    if (displayTaskHandle != NULL) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = NULL;
    }
    inConfigMode = false;
  }
}

void checkButtons() {
  if (digitalRead(BUTTON_1) == LOW && digitalRead(BUTTON_2) == LOW && digitalRead(BUTTON_3) == LOW) {
    if (!button1Pressed) {
      buttonPressTime = millis();
      button1Pressed = true;
      Serial.println("All 3 buttons pressed - hold for 3 seconds to factory reset");
    }
    
    if (millis() - buttonPressTime > 3000) {
      Serial.println("Factory reset triggered!");
      displayScrollText("FACTORY RESET", matrix.Color(255, 0, 0));
      
      tone(BUZZER, 1000, 200);
      delay(300);
      tone(BUZZER, 1000, 200);
      
      delay(1000);
      
      // Clear all preferences
      preferences.begin("tc001", false);
      preferences.clear();
      preferences.end();
      
      WiFiManager wifiManager;
      wifiManager.resetSettings();
      ESP.restart();
    }
  } else {
    button1Pressed = false;
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  
  String apName = "TC001-" + deviceID;
  inConfigMode = true;
  configModeMessage = "CONNECT TO: " + apName;
  
  // Reset scroll position for continuous scrolling
  scrollX = MATRIX_WIDTH;
  
  // Create FreeRTOS task for display updates while in config portal
  if (displayTaskHandle == NULL) {
    xTaskCreate(
      displayUpdateTask,      // Task function
      "DisplayTask",          // Task name
      4096,                   // Stack size
      NULL,                   // Parameters
      1,                      // Priority
      &displayTaskHandle      // Task handle
    );
    Serial.println("Display update task created");
  }
}

void setupWebServer() {
  // Root page
  server.on("/", HTTP_GET, handleRoot);
  
  // Configuration page
  server.on("/config", HTTP_GET, handleConfigPage);
  
  // Save configuration
  server.on("/save", HTTP_POST, handleSaveConfig);
  
  // Test API connection
  server.on("/test", HTTP_GET, handleTestAPI);
  
  // Factory reset endpoint
  server.on("/reset", HTTP_GET, handleFactoryReset);
  
  // Status endpoint (JSON)
  server.on("/status", HTTP_GET, handleStatus);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; border-bottom: 2px solid #4CAF50; padding-bottom: 10px; }";
  html += ".info { background: #e8f5e9; padding: 15px; border-radius: 5px; margin: 10px 0; }";
  html += ".label { font-weight: bold; color: #666; }";
  html += ".value { color: #2196F3; font-size: 1.2em; }";
  html += ".button { background: #4CAF50; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 1em; margin: 10px 5px; text-decoration: none; display: inline-block; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.danger { background: #f44336; }";
  html += ".button.danger:hover { background: #d32f2f; }";
  html += ".status { padding: 15px; border-radius: 5px; margin: 10px 0; }";
  html += ".status.ok { background: #e8f5e9; color: #2e7d32; }";
  html += ".status.error { background: #ffebee; color: #c62828; }";
  html += ".status.warning { background: #fff8e1; color: #f57f17; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>" + deviceName + "</h1>";
  
  html += "<div class='info'>";
  html += "<p><span class='label'>Device ID:</span> <span class='value'>" + deviceID + "</span></p>";
  html += "<p><span class='label'>IP Address:</span> <span class='value'>" + ipAddress + "</span></p>";
  html += "<p><span class='label'>WiFi SSID:</span> <span class='value'>" + String(WiFi.SSID()) + "</span></p>";
  html += "<p><span class='label'>Signal:</span> <span class='value'>" + String(WiFi.RSSI()) + " dBm</span></p>";
  html += "</div>";
  
  String statusClass = "warning";
  String statusText = "Not Configured";
  
  if (apiConfigured) {
    if (lastError.length() > 0) {
      statusClass = "error";
      statusText = "Error: " + lastError;
    } else {
      statusClass = "ok";
      statusText = "Current Value: " + displayPrefix + currentValue;
    }
  }
  
  html += "<div class='status " + statusClass + "'>";
  html += "<p><strong>API Status:</strong> " + statusText + "</p>";
  if (apiConfigured) {
    html += "<p><strong>Polling Interval:</strong> " + String(pollingInterval) + " seconds</p>";
    html += "<p><strong>Display Mode:</strong> " + String(scrollEnabled ? "Scrolling" : "Static") + "</p>";
    html += "<p><strong>Icon:</strong> " + String(iconEnabled ? "Enabled" : "Disabled") + "</p>";
  }
  html += "</div>";
  
  html += "<div style='margin: 20px 0;'>";
  html += "<a href='/config' class='button'>Configure API</a>";
  html += "<button class='button danger' onclick='factoryReset()'>Factory Reset</button>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function factoryReset() {";
  html += "  if(confirm('Are you sure? This will erase all settings and WiFi credentials.')) {";
  html += "    fetch('/reset').then(() => {";
  html += "      alert('Device is resetting...');";
  html += "    });";
  html += "  }";
  html += "}";
  html += "</script>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfigPage() {
  String maskedKey = "";
  if (apiKey.length() > 6) {
    maskedKey = String("*").substring(0, apiKey.length() - 6) + apiKey.substring(apiKey.length() - 6);
  } else if (apiKey.length() > 0) {
    maskedKey = "******";
  }
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; border-bottom: 2px solid #4CAF50; padding-bottom: 10px; }";
  html += ".form-group { margin: 15px 0; }";
  html += "label { display: block; font-weight: bold; color: #666; margin-bottom: 5px; }";
  html += "input[type='text'], input[type='password'], input[type='number'], textarea { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; font-size: 1em; font-family: monospace; }";
  html += "input[type='checkbox'] { width: auto; margin-right: 8px; }";
  html += "textarea { min-height: 100px; resize: vertical; }";
  html += ".button { background: #4CAF50; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 1em; margin: 10px 5px; }";
  html += ".button:hover { background: #45a049; }";
  html += ".button.secondary { background: #2196F3; }";
  html += ".button.secondary:hover { background: #0b7dda; }";
  html += ".help { font-size: 0.9em; color: #666; margin-top: 5px; }";
  html += ".example { background: #f5f5f5; padding: 10px; border-radius: 5px; margin-top: 10px; font-family: monospace; font-size: 0.85em; }";
  html += "#iconPreview { display: inline-block; margin: 10px 0; border: 1px solid #ddd; }";
  html += ".icon-pixel { width: 20px; height: 20px; display: inline-block; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>API Configuration</h1>";
  
  html += "<form method='POST' action='/save'>";
  
  html += "<div class='form-group'>";
  html += "<label>API Endpoint URL:</label>";
  html += "<input type='text' name='apiUrl' value='" + apiEndpoint + "' placeholder='https://api.example.com/data' required>";
  html += "<p class='help'>Full URL to your API endpoint</p>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>API Header Name:</label>";
  html += "<input type='text' name='apiHeader' value='" + apiHeaderName + "' placeholder='APIKey'>";
  html += "<p class='help'>Header name for authentication (e.g., APIKey, Authorization, X-API-Key)</p>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>API Key:</label>";
  html += "<input type='password' name='apiKey' value='" + apiKey + "' placeholder='Your API key'>";
  html += "<p class='help'>Leave blank if no authentication required. Current: " + (apiKey.length() > 0 ? maskedKey : "Not set") + "</p>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>JSON Path:</label>";
  html += "<input type='text' name='jsonPath' value='" + jsonPath + "' placeholder='data.value' required>";
  html += "<p class='help'>Path to the value in the JSON response</p>";
  html += "<div class='example'>Examples:<br>count<br>data.unassigned<br>TC001MatrixDisplay[0].OpenRequests<br>results[0].value<br><strong>Array filtering:</strong><br>OverdueWorkflows[Username=Binai Prasad].Overdue<br>users[name=John].age</div>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Display Prefix (optional):</label>";
  html += "<input type='text' name='prefix' value='" + displayPrefix + "' placeholder='Tickets: '>";
  html += "<p class='help'>Text to show before the value (e.g., 'Tickets: ' will display as 'Tickets: 42')</p>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Display Suffix (optional):</label>";
  html += "<input type='text' name='suffix' value='" + displaySuffix + "' placeholder=' items'>";
  html += "<p class='help'>Text to show after the value (e.g., ' items' will display as '42 items')</p>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>";
  html += "<input type='checkbox' name='scroll' value='1' " + String(scrollEnabled ? "checked" : "") + "> Enable Scrolling";
  html += "</label>";
  html += "<p class='help'>When checked, content scrolls continuously. When unchecked, content is centered and static.</p>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Icon Data (optional):</label>";
  html += "<textarea name='iconData' id='iconData' placeholder='[[255,0,0],[0,255,0],...]'>" + iconData + "</textarea>";
  html += "<p class='help'>8x8 icon as JSON array (64 pixels). Format: [[r,g,b],[r,g,b],...] Leave blank to disable icon.</p>";
  html += "<div class='example'>Create icons using any pixel art tool that exports RGB arrays, or manually create the 64-pixel array.</div>";
  html += "<div id='iconPreview'></div>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Polling Interval (seconds):</label>";
  html += "<input type='number' name='interval' value='" + String(pollingInterval) + "' min='5' max='3600'>";
  html += "<p class='help'>How often to poll the API (5-3600 seconds)</p>";
  html += "</div>";
  
  html += "<button type='submit' class='button'>Save Configuration</button>";
  html += "<button type='button' class='button secondary' onclick='testAPI()'>Test Connection</button>";
  html += "<a href='/' class='button secondary'>Cancel</a>";
  
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
  scrollEnabled = server.hasArg("scroll"); // Checkbox: present = checked, absent = unchecked
  iconData = server.arg("iconData");
  
  if (pollingInterval < 5) pollingInterval = 5;
  if (pollingInterval > 3600) pollingInterval = 3600;
  
  // Parse icon data if provided, otherwise clear icon
  if (iconData.length() > 0) {
    parseIconData(iconData);
  } else {
    iconEnabled = false;
  }
  
  saveConfiguration();
  
  // Trigger immediate API poll and reset scroll
  lastAPICall = 0;
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
  json += "\"last_error\":\"" + lastError + "\"";
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
