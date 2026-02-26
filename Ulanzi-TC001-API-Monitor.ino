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
String buildNumber = "v1.1.0";

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

// Screen configuration
#define MAX_SCREENS 5

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

// Screen data structure
struct Screen {
  String name;
  String apiEndpoint;
  String apiKey;
  String apiHeaderName;
  String jsonPath;
  String displayPrefix;
  String displaySuffix;
  int pollingInterval;
  bool scrollEnabled;
  String iconData;
  // Runtime (not persisted)
  bool iconEnabled;
  uint16_t iconPixels[64];
  String currentValue;
  String lastError;
  unsigned long lastAPICall;
  bool apiConfigured;
};

Screen screens[MAX_SCREENS];
int numScreens = 0;
int activeScreen = 0;
bool autoRotate = false;
int rotateInterval = 10; // seconds between auto-rotation
unsigned long lastRotateTime = 0;

// Brightness configuration
bool autoBrightness = false; // false = manual, true = auto (light sensor)
int manualBrightness = 40; // 0-255, used when autoBrightness is false
unsigned long lastBrightnessUpdate = 0;
const int brightnessUpdateInterval = 100; // Update brightness every 100ms

// Battery monitoring
float batteryVoltage = 0.0;
int batteryPercentage = 0;
unsigned long lastBatteryUpdate = 0;
const unsigned long batteryUpdateInterval = 60000; // Update battery every 60 seconds
bool showBatteryRequested = false;
unsigned long batteryDisplayTime = 0;
const unsigned long batteryDisplayDuration = 3000; // Show battery for 3 seconds

// Authentication
String adminPassword = "ulanzitc001"; // Default password
bool isAuthenticated = false;
unsigned long authTimestamp = 0;
const unsigned long authTimeout = 1800000; // 30 minutes in milliseconds

// Button state tracking
enum ButtonCombo {
  COMBO_NONE,
  COMBO_BTN1,
  COMBO_BTN2,
  COMBO_BTN3,
  COMBO_BTN23,
  COMBO_BTN123
};

ButtonCombo currentCombo = COMBO_NONE;
unsigned long comboPressStartTime = 0;
bool comboActionExecuted = false;

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
  
  // Poll active screen immediately if configured
  if (numScreens > 0 && screens[activeScreen].apiConfigured) {
    Serial.println("Performing initial API poll for active screen...");
    pollScreenAPI(activeScreen);
    screens[activeScreen].lastAPICall = millis();
  }

  // Reset scroll position and auto-rotate timer
  scrollX = MATRIX_WIDTH;
  lastRotateTime = millis();
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

  // Auto-rotate screens
  if (autoRotate && numScreens > 1 && !showBatteryRequested) {
    if (millis() - lastRotateTime > (unsigned long)(rotateInterval * 1000)) {
      nextScreen();
      lastRotateTime = millis();
    }
  }

  // Poll all configured screens at their intervals
  for (int i = 0; i < numScreens; i++) {
    if (screens[i].apiConfigured && (millis() - screens[i].lastAPICall > (unsigned long)(screens[i].pollingInterval * 1000))) {
      pollScreenAPI(i);
      screens[i].lastAPICall = millis();
    }
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
  
  bool useScroll = (numScreens > 0) ? screens[activeScreen].scrollEnabled : true;
  if (useScroll) {
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

  // Load global settings
  autoBrightness = preferences.getBool("autoBrightness", false);
  manualBrightness = preferences.getInt("brightness", 40);
  adminPassword = preferences.getString("adminPassword", "ulanzitc001");
  autoRotate = preferences.getBool("autoRotate", false);
  rotateInterval = preferences.getInt("rotateIntv", 10);
  numScreens = preferences.getInt("numScreens", 0);
  activeScreen = preferences.getInt("activeScr", 0);

  // Migration from old single-screen format
  if (numScreens == 0) {
    String oldEndpoint = preferences.getString("apiUrl", "");
    if (oldEndpoint.length() > 0) {
      Serial.println("Migrating old single-screen config to multi-screen format...");
      screens[0].name = "Screen 1";
      screens[0].apiEndpoint = oldEndpoint;
      screens[0].apiKey = preferences.getString("apiKey", "");
      screens[0].apiHeaderName = preferences.getString("apiHeader", "APIKey");
      screens[0].jsonPath = preferences.getString("jsonPath", "");
      screens[0].displayPrefix = preferences.getString("prefix", "");
      screens[0].displaySuffix = preferences.getString("suffix", "");
      screens[0].pollingInterval = preferences.getInt("interval", 60);
      screens[0].scrollEnabled = preferences.getBool("scroll", true);
      screens[0].iconData = preferences.getString("iconData", "");
      numScreens = 1;
      activeScreen = 0;

      // Remove old keys
      preferences.remove("apiUrl");
      preferences.remove("apiKey");
      preferences.remove("apiHeader");
      preferences.remove("jsonPath");
      preferences.remove("prefix");
      preferences.remove("suffix");
      preferences.remove("interval");
      preferences.remove("scroll");
      preferences.remove("iconData");

      // Save in new format
      preferences.putInt("numScreens", numScreens);
      preferences.putInt("activeScr", activeScreen);
      saveScreenToPrefs(0);

      Serial.println("Migration complete");
    }
  }

  // Load all screens
  for (int i = 0; i < numScreens; i++) {
    String idx = String(i);
    screens[i].name = preferences.getString(("s" + idx + "name").c_str(), "Screen " + String(i + 1));
    screens[i].apiEndpoint = preferences.getString(("s" + idx + "url").c_str(), "");
    screens[i].apiKey = preferences.getString(("s" + idx + "key").c_str(), "");
    screens[i].apiHeaderName = preferences.getString(("s" + idx + "hdr").c_str(), "APIKey");
    screens[i].jsonPath = preferences.getString(("s" + idx + "path").c_str(), "");
    screens[i].displayPrefix = preferences.getString(("s" + idx + "pfx").c_str(), "");
    screens[i].displaySuffix = preferences.getString(("s" + idx + "sfx").c_str(), "");
    screens[i].pollingInterval = preferences.getInt(("s" + idx + "intv").c_str(), 60);
    screens[i].scrollEnabled = preferences.getBool(("s" + idx + "scrl").c_str(), true);
    screens[i].iconData = preferences.getString(("s" + idx + "icon").c_str(), "");

    // Initialize runtime state
    screens[i].currentValue = "---";
    screens[i].lastError = "";
    screens[i].lastAPICall = 0;
    screens[i].apiConfigured = (screens[i].apiEndpoint.length() > 0 && screens[i].jsonPath.length() > 0);
    screens[i].iconEnabled = false;

    if (screens[i].iconData.length() > 0) {
      parseIconData(screens[i].iconData, screens[i].iconPixels, screens[i].iconEnabled);
    }
  }

  preferences.end();

  // Validate activeScreen
  if (activeScreen >= numScreens && numScreens > 0) activeScreen = 0;
  if (numScreens == 0) activeScreen = 0;

  // Apply brightness setting
  if (autoBrightness) {
    updateBrightness();
  } else {
    matrix.setBrightness(manualBrightness);
  }

  Serial.println("Configuration loaded:");
  Serial.println("  Screens: " + String(numScreens));
  Serial.println("  Active: " + String(activeScreen));
  Serial.println("  Auto Rotate: " + String(autoRotate ? "Yes" : "No"));
  Serial.println("  Rotate Interval: " + String(rotateInterval) + "s");
  for (int i = 0; i < numScreens; i++) {
    Serial.println("  Screen " + String(i) + ": " + screens[i].name);
    Serial.println("    Endpoint: " + screens[i].apiEndpoint);
    Serial.println("    Configured: " + String(screens[i].apiConfigured ? "Yes" : "No"));
  }
}

void saveScreenToPrefs(int index) {
  // Assumes preferences namespace is already open
  String idx = String(index);
  preferences.putString(("s" + idx + "name").c_str(), screens[index].name);
  preferences.putString(("s" + idx + "url").c_str(), screens[index].apiEndpoint);
  preferences.putString(("s" + idx + "key").c_str(), screens[index].apiKey);
  preferences.putString(("s" + idx + "hdr").c_str(), screens[index].apiHeaderName);
  preferences.putString(("s" + idx + "path").c_str(), screens[index].jsonPath);
  preferences.putString(("s" + idx + "pfx").c_str(), screens[index].displayPrefix);
  preferences.putString(("s" + idx + "sfx").c_str(), screens[index].displaySuffix);
  preferences.putInt(("s" + idx + "intv").c_str(), screens[index].pollingInterval);
  preferences.putBool(("s" + idx + "scrl").c_str(), screens[index].scrollEnabled);
  preferences.putString(("s" + idx + "icon").c_str(), screens[index].iconData);
}

void removeScreenFromPrefs(int index) {
  // Assumes preferences namespace is already open
  String idx = String(index);
  preferences.remove(("s" + idx + "name").c_str());
  preferences.remove(("s" + idx + "url").c_str());
  preferences.remove(("s" + idx + "key").c_str());
  preferences.remove(("s" + idx + "hdr").c_str());
  preferences.remove(("s" + idx + "path").c_str());
  preferences.remove(("s" + idx + "pfx").c_str());
  preferences.remove(("s" + idx + "sfx").c_str());
  preferences.remove(("s" + idx + "intv").c_str());
  preferences.remove(("s" + idx + "scrl").c_str());
  preferences.remove(("s" + idx + "icon").c_str());
}

void saveAllConfiguration() {
  preferences.begin("tc001", false);

  preferences.putBool("autoBrightness", autoBrightness);
  preferences.putInt("brightness", manualBrightness);
  preferences.putString("adminPassword", adminPassword);
  preferences.putBool("autoRotate", autoRotate);
  preferences.putInt("rotateIntv", rotateInterval);
  preferences.putInt("numScreens", numScreens);
  preferences.putInt("activeScr", activeScreen);

  for (int i = 0; i < numScreens; i++) {
    saveScreenToPrefs(i);
  }

  preferences.end();
  Serial.println("All configuration saved");
}

// HTML escape function to prevent HTML injection and attribute breaking
String htmlEscape(const String& str) {
  String escaped = str;
  escaped.replace("&", "&amp;");
  escaped.replace("\"", "&quot;");
  escaped.replace("'", "&#39;");
  escaped.replace("<", "&lt;");
  escaped.replace(">", "&gt;");
  return escaped;
}

// URL encode function - encodes query parameters in URLs
String urlEncode(const String& url) {
  // Find the query string start (after ?)
  int queryStart = url.indexOf('?');
  if (queryStart == -1) {
    // No query string, return as-is
    return url;
  }

  // Split into base URL and query string
  String baseUrl = url.substring(0, queryStart + 1);
  String queryString = url.substring(queryStart + 1);

  // Encode the query string
  String encoded = "";
  for (unsigned int i = 0; i < queryString.length(); i++) {
    char c = queryString.charAt(i);

    // Characters that should NOT be encoded in query strings
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
        c == '=' || c == '&') {
      // These are safe or structural characters
      encoded += c;
    } else if (c == ' ') {
      // Encode space as %20
      encoded += "%20";
    } else {
      // Encode other characters as %XX
      encoded += '%';
      char hex[3];
      sprintf(hex, "%02X", c);
      encoded += hex;
    }
  }

  return baseUrl + encoded;
}

void checkButtons() {
  // Read all button states
  bool btn1 = (digitalRead(BUTTON_1) == LOW);
  bool btn2 = (digitalRead(BUTTON_2) == LOW);
  bool btn3 = (digitalRead(BUTTON_3) == LOW);

  // Determine which combination is currently pressed (priority: most buttons first)
  ButtonCombo pressedCombo = COMBO_NONE;
  if (btn1 && btn2 && btn3) {
    pressedCombo = COMBO_BTN123;
  } else if (btn2 && btn3) {
    pressedCombo = COMBO_BTN23;
  } else if (btn1) {
    pressedCombo = COMBO_BTN1;
  } else if (btn2) {
    pressedCombo = COMBO_BTN2;
  } else if (btn3) {
    pressedCombo = COMBO_BTN3;
  }

  // Handle state transitions
  if (pressedCombo != COMBO_NONE) {
    // Buttons are pressed
    if (currentCombo == COMBO_NONE) {
      // New press started
      currentCombo = pressedCombo;
      comboPressStartTime = millis();
      comboActionExecuted = false;
      Serial.println("Button press detected: " + String(pressedCombo));
    } else if (currentCombo != pressedCombo) {
      // Combination changed (e.g., started with btn2, then pressed btn3 too)
      currentCombo = pressedCombo;
      comboPressStartTime = millis();
      comboActionExecuted = false;
      Serial.println("Button combo changed to: " + String(pressedCombo));
    } else {
      // Same combo still held - check for long press actions
      unsigned long holdTime = millis() - comboPressStartTime;

      if (!comboActionExecuted) {
        switch (currentCombo) {
          case COMBO_BTN123:
            // All 3 buttons - Factory reset after 3 seconds
            if (holdTime >= 3000) {
              Serial.println("Factory reset triggered (3s hold)");
              performFactoryReset();
              comboActionExecuted = true;
            }
            break;

          case COMBO_BTN23:
            // Button 2 + 3 - Show battery after 500ms
            if (holdTime >= 500) {
              Serial.println("Battery display triggered (0.5s hold)");
              showBatteryOnDisplay();
              comboActionExecuted = true;
            }
            break;

          case COMBO_BTN2:
            // Button 2 solo - Manual API refresh after 1 second
            if (holdTime >= 1000) {
              Serial.println("Manual API refresh triggered (1s hold)");
              if (numScreens > 0 && screens[activeScreen].apiConfigured) {
                pollScreenAPI(activeScreen);
                screens[activeScreen].lastAPICall = millis();
              }
              comboActionExecuted = true;
            }
            break;

          default:
            // Other combos not yet implemented
            break;
        }
      }
    }
  } else {
    // No buttons pressed - handle release events
    if (currentCombo != COMBO_NONE) {
      unsigned long holdTime = millis() - comboPressStartTime;
      Serial.println("Button released after " + String(holdTime) + "ms");

      // Short press actions (only if no long press action was executed)
      if (!comboActionExecuted) {
        if (currentCombo == COMBO_BTN1 && holdTime < 500) {
          Serial.println("Short press Button 1 - previous screen");
          prevScreen();
        } else if (currentCombo == COMBO_BTN3 && holdTime < 500) {
          Serial.println("Short press Button 3 - next screen");
          nextScreen();
        }
      }

      // Reset state
      currentCombo = COMBO_NONE;
      comboActionExecuted = false;
    }
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
  
  // Create task for continuous display updates (core 1 to avoid WiFi task starvation on core 0)
  xTaskCreatePinnedToCore(
    displayUpdateTask,
    "DisplayTask",
    4096,
    NULL,
    1,
    &displayTaskHandle,
    1
  );
  
  Serial.println("Display update task created");
  scrollX = MATRIX_WIDTH;
}

// ============================================
// Screen Navigation Functions
// ============================================

void nextScreen() {
  if (numScreens <= 1) return;
  activeScreen = (activeScreen + 1) % numScreens;
  onScreenSwitch();
}

void prevScreen() {
  if (numScreens <= 1) return;
  activeScreen = (activeScreen - 1 + numScreens) % numScreens;
  onScreenSwitch();
}

void switchToScreen(int index) {
  if (index < 0 || index >= numScreens) return;
  if (index == activeScreen) return;
  activeScreen = index;
  onScreenSwitch();
}

void onScreenSwitch() {
  scrollX = MATRIX_WIDTH;
  lastRotateTime = millis(); // Reset auto-rotate timer

  // Save active screen preference
  preferences.begin("tc001", false);
  preferences.putInt("activeScr", activeScreen);
  preferences.end();

  Serial.println("Switched to screen " + String(activeScreen) + ": " + screens[activeScreen].name);
}

// ============================================
// API Polling Functions
// ============================================

void pollScreenAPI(int index) {
  if (index < 0 || index >= numScreens) return;
  Screen& scr = screens[index];
  if (!scr.apiConfigured) return;

  const int MAX_RETRIES = 2;
  const int TIMEOUT_MS = 10000;
  int retryCount = 0;
  bool success = false;

  while (retryCount <= MAX_RETRIES && !success) {
    if (retryCount > 0) {
      Serial.println("Retry attempt " + String(retryCount) + " of " + String(MAX_RETRIES));
      delay(1000);
    }

    String encodedUrl = urlEncode(scr.apiEndpoint);

    HTTPClient http;
    http.begin(encodedUrl);
    http.setTimeout(TIMEOUT_MS);

    if (scr.apiKey.length() > 0) {
      http.addHeader(scr.apiHeaderName, scr.apiKey);
    }

    Serial.println("[Screen " + String(index) + "] Polling: " + scr.apiEndpoint);
    if (encodedUrl != scr.apiEndpoint) {
      Serial.println("Encoded URL: " + encodedUrl);
    }
    int httpCode = http.GET();

    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        Serial.println("[Screen " + String(index) + "] Response: " + String(payload.length()) + " bytes");

        String value = extractJSONValue(payload, scr.jsonPath);

        if (value.length() > 0) {
          scr.currentValue = scr.displayPrefix + value + scr.displaySuffix;
          scr.lastError = "";
          Serial.println("[Screen " + String(index) + "] Value: " + scr.currentValue);
          if (index == activeScreen) {
            scrollX = MATRIX_WIDTH;
          }
          success = true;
        } else {
          scr.currentValue = "PATH ERROR";
          scr.lastError = "Could not extract value from JSON path";
          success = true;
        }
      } else {
        scr.currentValue = "HTTP " + String(httpCode);
        scr.lastError = "HTTP error: " + String(httpCode);
        success = true;
      }
    } else {
      String errorMsg = http.errorToString(httpCode);
      Serial.println("[Screen " + String(index) + "] Connection failed: " + errorMsg);

      if (retryCount == MAX_RETRIES) {
        scr.currentValue = "CONN FAIL";
        scr.lastError = errorMsg;
      }
    }

    http.end();
    retryCount++;
  }

  if (!success) {
    Serial.println("[Screen " + String(index) + "] Failed after " + String(MAX_RETRIES + 1) + " attempts");
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

  // Handle config mode
  if (inConfigMode) {
    matrix.setTextColor(matrix.Color(255, 165, 0));
    int16_t textWidth = configModeMessage.length() * 6;
    matrix.setCursor(scrollX, 0);
    matrix.print(configModeMessage);
    matrix.show();
    scrollX--;
    if (scrollX < -textWidth) scrollX = MATRIX_WIDTH;
    return;
  }

  // No screens configured
  if (numScreens == 0) {
    matrix.setTextColor(matrix.Color(255, 165, 0));
    String msg = "NO API";
    int16_t x1, y1;
    uint16_t w, h;
    matrix.getTextBounds(msg.c_str(), 0, 0, &x1, &y1, &w, &h);
    matrix.setCursor((MATRIX_WIDTH - w) / 2, 0);
    matrix.print(msg);
    matrix.show();
    return;
  }

  Screen& scr = screens[activeScreen];

  uint16_t color;
  if (scr.lastError.length() > 0) {
    color = matrix.Color(255, 0, 0);
  } else {
    color = matrix.Color(0, 255, 0);
  }
  matrix.setTextColor(color);

  if (scr.scrollEnabled) {
    int iconOffset = scr.iconEnabled ? (ICON_WIDTH + 1) : 0;
    int16_t textWidth = scr.currentValue.length() * 6;

    if (scr.iconEnabled && scrollX < ICON_WIDTH) {
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < ICON_WIDTH; x++) {
          if (scrollX + x >= 0 && scrollX + x < MATRIX_WIDTH) {
            matrix.drawPixel(scrollX + x, y, scr.iconPixels[y * 8 + x]);
          }
        }
      }
    }

    matrix.setCursor(scrollX + iconOffset, 0);
    matrix.print(scr.currentValue);
    matrix.show();

    scrollX--;
    if (scrollX < -(textWidth + iconOffset)) {
      scrollX = MATRIX_WIDTH;
    }
  } else {
    int displayWidth = scr.iconEnabled ? TEXT_WIDTH : MATRIX_WIDTH;
    int xOffset = scr.iconEnabled ? ICON_WIDTH : 0;

    if (scr.iconEnabled) {
      for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
          matrix.drawPixel(x, y, scr.iconPixels[y * 8 + x]);
        }
      }
    }

    int16_t x1, y1;
    uint16_t w, h;
    matrix.getTextBounds(scr.currentValue.c_str(), 0, 0, &x1, &y1, &w, &h);

    int16_t centerX = xOffset + (displayWidth - w) / 2;
    if (centerX < xOffset) centerX = xOffset;

    matrix.setCursor(centerX, 0);
    matrix.print(scr.currentValue);
    matrix.show();
  }
}

void parseIconData(const String& jsonData, uint16_t pixelArray[64], bool& enabled) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, jsonData);

  if (error) {
    Serial.print("Icon parse error: ");
    Serial.println(error.c_str());
    enabled = false;
    return;
  }

  if (!doc.is<JsonArray>()) {
    Serial.println("Icon data is not an array");
    enabled = false;
    return;
  }

  JsonArray pixels = doc.as<JsonArray>();
  if (pixels.size() != 64) {
    Serial.print("Icon must have 64 pixels, got: ");
    Serial.println(pixels.size());
    enabled = false;
    return;
  }

  for (int i = 0; i < 64; i++) {
    JsonArray pixel = pixels[i];
    if (!pixel || pixel.size() < 3) {
      Serial.print("Invalid pixel at index: ");
      Serial.println(i);
      enabled = false;
      return;
    }

    uint8_t r = pixel[0];
    uint8_t g = pixel[1];
    uint8_t b = pixel[2];

    pixelArray[i] = matrix.Color(r, g, b);
  }

  enabled = true;
  Serial.println("Icon parsed successfully (64 pixels)");
}

// ============================================
// Authentication Functions
// ============================================

bool checkAuth() {
  // Check if authenticated and session hasn't expired
  if (isAuthenticated && (millis() - authTimestamp < authTimeout)) {
    authTimestamp = millis(); // Refresh session on activity
    return true;
  }
  isAuthenticated = false;
  return false;
}

// Shared CSS for all pages (minified to save flash)
const char COMMON_CSS[] PROGMEM =
  "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0}"
  "h1{color:#333;border-bottom:2px solid #4CAF50;padding-bottom:10px}"
  "h2{color:#555;margin-top:20px}"
  ".container{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,.1)}"
  ".button{display:inline-block;background:#4CAF50;color:#fff;padding:10px 20px;text-decoration:none;border-radius:5px;margin:5px;border:none;cursor:pointer}"
  ".button:hover{background:#45a049}.button.secondary{background:#008CBA}.button.danger{background:#f44336}"
  ".button.small{padding:6px 12px;font-size:12px}"
  "label{display:block;margin-top:15px;font-weight:bold;color:#555}"
  "input[type='text'],input[type='password'],input[type='number'],textarea{width:100%;padding:10px;margin-top:5px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}"
  "textarea{min-height:60px;font-family:monospace}"
  "input[type='checkbox']{margin-top:10px}.checkbox-label{display:inline-block;margin-left:8px;font-weight:normal}"
  ".help{font-size:12px;color:#666;margin-top:5px;font-style:italic}"
  ".info-box{background:#e3f2fd;border-left:4px solid #2196F3;padding:15px;margin:15px 0;border-radius:5px}"
  ".warning-box{background:#fff3cd;border-left:4px solid #ffc107;padding:15px;margin:15px 0;border-radius:5px}"
  ".status{padding:15px;border-radius:5px;margin:10px 0}"
  ".status.ok,.status.success{background:#d4edda;color:#155724;border:1px solid #c3e6cb}"
  ".status.error{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb}"
  ".status.warning{background:#fff3cd;color:#856404;border:1px solid #ffeaa7}"
  ".icon-preview{margin-top:10px}.icon-pixel{width:15px;height:15px;display:inline-block;border:1px solid #ddd}";

String htmlHeader(const String& title) {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  if (title.length() > 0) html += "<title>" + title + " - " + deviceName + "</title>";
  html += "<style>";
  html += COMMON_CSS;
  return html;
}

void handleLogin() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Login - " + deviceName + "</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 0; padding: 0; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; display: flex; align-items: center; justify-content: center; }";
  html += ".login-container { background: white; padding: 40px; border-radius: 10px; box-shadow: 0 10px 25px rgba(0,0,0,0.2); max-width: 400px; width: 90%; }";
  html += "h1 { color: #333; margin-top: 0; text-align: center; }";
  html += "label { display: block; margin-top: 15px; font-weight: bold; color: #555; }";
  html += "input[type='password'] { width: 100%; padding: 12px; margin-top: 5px; border: 2px solid #ddd; border-radius: 5px; box-sizing: border-box; font-size: 16px; }";
  html += "input[type='password']:focus { border-color: #4CAF50; outline: none; }";
  html += ".button { display: block; width: 100%; background: #4CAF50; color: white; padding: 12px; text-decoration: none; border-radius: 5px; margin-top: 20px; border: none; cursor: pointer; font-size: 16px; font-weight: bold; }";
  html += ".button:hover { background: #45a049; }";
  html += ".error { background: #f8d7da; color: #721c24; padding: 10px; border-radius: 5px; margin-top: 10px; display: none; }";
  html += ".device-info { text-align: center; color: #666; font-size: 14px; margin-top: 20px; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='login-container'>";
  html += "<h1>TC001 Login</h1>";
  html += "<p class='device-info'>Device: " + deviceName + "<br>IP: " + ipAddress + "</p>";
  html += "<form method='POST' action='/login'>";
  html += "<label>Password:</label>";
  html += "<input type='password' name='password' placeholder='Enter password' required autofocus>";
  html += "<button type='submit' class='button'>Login</button>";
  html += "</form>";
  html += "<div class='error' id='error'>Invalid password!</div>";
  html += "</div>";

  // Show error if login failed
  if (server.hasArg("failed")) {
    html += "<script>document.getElementById('error').style.display='block';</script>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleLoginPost() {
  String password = server.arg("password");

  if (password == adminPassword) {
    isAuthenticated = true;
    authTimestamp = millis();
    Serial.println("Login successful");
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    Serial.println("Login failed - incorrect password");
    server.sendHeader("Location", "/login?failed=1");
    server.send(303);
  }
}

void handleLogout() {
  isAuthenticated = false;
  authTimestamp = 0;
  Serial.println("User logged out");
  server.sendHeader("Location", "/login");
  server.send(303);
}

bool requireAuth() {
  if (!checkAuth()) {
    server.sendHeader("Location", "/login");
    server.send(303);
    return false;
  }
  return true;
}

// ============================================
// Backup and Restore Functions
// ============================================

void handleBackupRestorePage() {
  if (!requireAuth()) return;

  String html = htmlHeader("Backup & Restore");
  html += ".container{max-width:700px}";
  html += ".button{padding:12px 24px;font-size:16px}.button.secondary:hover{background:#007399}";
  html += ".status{display:none}.status.success,.status.error{display:block}";
  html += "ul{line-height:1.8}input[type='file']{padding:10px;margin:10px 0}";
  html += ".section{margin:30px 0;padding:20px;background:#f9f9f9;border-radius:8px}";
  html += "</style></head><body>";

  html += "<div class='container'>";
  html += "<h1>Backup & Restore Configuration</h1>";

  html += "<div class='info-box'>";
  html += "<strong>About Backup & Restore</strong><br>";
  html += "This feature allows you to save and restore your device configuration. ";
  html += "Perfect for updating firmware or transferring settings between devices.";
  html += "</div>";

  // Backup Section
  html += "<div class='section'>";
  html += "<h2>Backup Configuration</h2>";
  html += "<p>Create a backup file containing your current settings:</p>";
  html += "<ul>";
  html += "<li>All screen configurations (endpoints, display settings, icons)</li>";
  html += "<li>Auto-rotation settings</li>";
  html += "<li>Brightness preferences</li>";
  html += "</ul>";
  html += "<div class='warning-box'>";
  html += "<strong>Note:</strong> For security reasons, the following are NOT included in backups:";
  html += "<ul style='margin: 5px 0;'>";
  html += "<li>WiFi credentials (SSID and password)</li>";
  html += "<li>Admin password</li>";
  html += "<li>API keys</li>";
  html += "</ul>";
  html += "</div>";
  html += "<button onclick='downloadBackup()' class='button'>Download Backup</button>";
  html += "</div>";

  // Restore Section
  html += "<div class='section'>";
  html += "<h2>Restore Configuration</h2>";
  html += "<p>Upload a previously saved backup file to restore your settings.</p>";
  html += "<div class='warning-box'>";
  html += "<strong>Important:</strong> The device will automatically restart after restoring to apply the new settings.";
  html += "</div>";
  html += "<form id='restoreForm' enctype='multipart/form-data'>";
  html += "<input type='file' id='backupFile' accept='.json' required>";
  html += "<button type='submit' class='button secondary'>Upload & Restore</button>";
  html += "</form>";
  html += "<div id='restoreStatus' class='status'></div>";
  html += "</div>";

  html += "<button onclick='location.href=\"/\"' class='button secondary'>Back to Home</button>";

  html += "</div>";

  // JavaScript
  html += "<script>";

  // Download backup function
  html += "function downloadBackup() {";
  html += "  console.log('Download backup button clicked');";
  html += "  fetch('/backup/download')";
  html += "    .then(response => {";
  html += "      console.log('Response received:', response.status);";
  html += "      if (!response.ok) {";
  html += "        throw new Error('HTTP ' + response.status);";
  html += "      }";
  html += "      return response.json();";
  html += "    })";
  html += "    .then(data => {";
  html += "      console.log('Data received:', data);";
  html += "      data.backup_date = new Date().toISOString();";
  html += "      const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });";
  html += "      const url = URL.createObjectURL(blob);";
  html += "      const a = document.createElement('a');";
  html += "      a.href = url;";
  html += "      a.download = 'tc001-backup-' + new Date().toISOString().split('T')[0] + '.json';";
  html += "      document.body.appendChild(a);";
  html += "      a.click();";
  html += "      document.body.removeChild(a);";
  html += "      URL.revokeObjectURL(url);";
  html += "      console.log('Download initiated');";
  html += "    })";
  html += "    .catch(err => {";
  html += "      console.error('Error:', err);";
  html += "      alert('Error creating backup: ' + err.message);";
  html += "    });";
  html += "}";

  // Restore form submission
  html += "document.getElementById('restoreForm').addEventListener('submit', function(e) {";
  html += "  e.preventDefault();";
  html += "  const fileInput = document.getElementById('backupFile');";
  html += "  const file = fileInput.files[0];";
  html += "  if (!file) {";
  html += "    alert('Please select a backup file');";
  html += "    return;";
  html += "  }";
  html += "  const reader = new FileReader();";
  html += "  reader.onload = function(e) {";
  html += "    try {";
  html += "      const config = JSON.parse(e.target.result);";
  html += "      const currentVersion = '" + buildNumber + "';";
  html += "      const backupVersion = config.version || 'unknown';";
  html += "      if (backupVersion !== currentVersion) {";
  html += "        const message = 'Version mismatch detected:\\\\n\\\\n' +";
  html += "                       'Backup version: ' + backupVersion + '\\\\n' +";
  html += "                       'Current firmware: ' + currentVersion + '\\\\n\\\\n' +";
  html += "                       'Settings may not be fully compatible. Continue anyway?';";
  html += "        if (!confirm(message)) {";
  html += "          return;";
  html += "        }";
  html += "      }";
  html += "      fetch('/backup/restore', {";
  html += "        method: 'POST',";
  html += "        headers: { 'Content-Type': 'application/json' },";
  html += "        body: JSON.stringify(config)";
  html += "      })";
  html += "      .then(response => response.json())";
  html += "      .then(data => {";
  html += "        if (data.success) {";
  html += "          document.getElementById('restoreStatus').className = 'status success';";
  html += "          document.getElementById('restoreStatus').innerHTML = '<strong>Success!</strong><br>' + data.message + '<br><br>Device will restart in 3 seconds...';";
  html += "          setTimeout(() => { window.location.href = '/'; }, 3000);";
  html += "        } else {";
  html += "          document.getElementById('restoreStatus').className = 'status error';";
  html += "          document.getElementById('restoreStatus').innerHTML = '<strong>Error!</strong><br>' + data.message;";
  html += "        }";
  html += "      })";
  html += "      .catch(err => {";
  html += "        document.getElementById('restoreStatus').className = 'status error';";
  html += "        document.getElementById('restoreStatus').innerHTML = '<strong>Error!</strong><br>Failed to restore: ' + err;";
  html += "      });";
  html += "    } catch(err) {";
  html += "      alert('Invalid backup file format: ' + err);";
  html += "    }";
  html += "  };";
  html += "  reader.readAsText(file);";
  html += "});";

  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleBackupDownload() {
  if (!requireAuth()) return;

  DynamicJsonDocument doc(16384);

  doc["version"] = buildNumber;
  doc["device_id"] = deviceID;
  doc["backup_date"] = "";

  // Global settings
  doc["auto_brightness"] = autoBrightness;
  doc["manual_brightness"] = manualBrightness;
  doc["auto_rotate"] = autoRotate;
  doc["rotate_interval"] = rotateInterval;

  // Screens array (excluding API keys for security)
  JsonArray screensArr = doc.createNestedArray("screens");
  for (int i = 0; i < numScreens; i++) {
    JsonObject s = screensArr.createNestedObject();
    s["name"] = screens[i].name;
    s["api_endpoint"] = screens[i].apiEndpoint;
    s["api_header_name"] = screens[i].apiHeaderName;
    s["json_path"] = screens[i].jsonPath;
    s["display_prefix"] = screens[i].displayPrefix;
    s["display_suffix"] = screens[i].displaySuffix;
    s["polling_interval"] = screens[i].pollingInterval;
    s["scroll_enabled"] = screens[i].scrollEnabled;
    s["icon_data"] = screens[i].iconData;
  }

  String output;
  serializeJson(doc, output);

  server.send(200, "application/json", output);
  Serial.println("Backup created and downloaded");
}

void handleBackupRestore() {
  if (!requireAuth()) return;

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    String response = "{\"success\":false,\"message\":\"Invalid JSON format\"}";
    server.send(400, "application/json", response);
    Serial.println("Restore failed: Invalid JSON");
    return;
  }

  // Global settings
  if (doc.containsKey("auto_brightness")) autoBrightness = doc["auto_brightness"];
  if (doc.containsKey("manual_brightness")) manualBrightness = doc["manual_brightness"];
  if (doc.containsKey("auto_rotate")) autoRotate = doc["auto_rotate"];
  if (doc.containsKey("rotate_interval")) rotateInterval = doc["rotate_interval"];

  // Check for new multi-screen format
  if (doc.containsKey("screens")) {
    JsonArray screensArr = doc["screens"].as<JsonArray>();

    // Clear existing screens from NVS
    preferences.begin("tc001", false);
    for (int i = 0; i < numScreens; i++) {
      removeScreenFromPrefs(i);
    }
    preferences.end();

    numScreens = min((int)screensArr.size(), MAX_SCREENS);
    activeScreen = 0;

    for (int i = 0; i < numScreens; i++) {
      JsonObject s = screensArr[i];
      String defaultName = "Screen " + String(i + 1);
      screens[i].name = s["name"] | defaultName.c_str();
      screens[i].apiEndpoint = s["api_endpoint"] | "";
      screens[i].apiHeaderName = s["api_header_name"] | "APIKey";
      screens[i].jsonPath = s["json_path"] | "";
      screens[i].displayPrefix = s["display_prefix"] | "";
      screens[i].displaySuffix = s["display_suffix"] | "";
      screens[i].pollingInterval = s["polling_interval"] | 60;
      screens[i].scrollEnabled = s["scroll_enabled"] | true;
      screens[i].iconData = s["icon_data"] | "";
      screens[i].apiConfigured = (screens[i].apiEndpoint.length() > 0 && screens[i].jsonPath.length() > 0);
    }
  } else if (doc.containsKey("api_endpoint")) {
    // Legacy single-screen backup format
    numScreens = 1;
    activeScreen = 0;
    screens[0].name = "Screen 1";
    screens[0].apiEndpoint = doc["api_endpoint"] | "";
    screens[0].apiHeaderName = doc["api_header_name"] | "APIKey";
    screens[0].jsonPath = doc["json_path"] | "";
    screens[0].displayPrefix = doc["display_prefix"] | "";
    screens[0].displaySuffix = doc["display_suffix"] | "";
    screens[0].pollingInterval = doc["polling_interval"] | 60;
    screens[0].scrollEnabled = doc["scroll_enabled"] | true;
    screens[0].iconData = doc["icon_data"] | "";
    screens[0].apiConfigured = (screens[0].apiEndpoint.length() > 0 && screens[0].jsonPath.length() > 0);
  }

  // Save everything
  saveAllConfiguration();

  String response = "{\"success\":true,\"message\":\"Configuration restored successfully\"}";
  server.send(200, "application/json", response);

  Serial.println("Configuration restored from backup");

  delay(1000);
  ESP.restart();
}

void setupWebServer() {
  // Public routes (no auth required)
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", handleLogout);
  server.on("/status", handleStatus);
  server.on("/favicon.ico", handleFavicon);

  // Protected routes (auth required)
  server.on("/", handleRoot);
  server.on("/screens", handleScreensPage);
  server.on("/screens/edit", handleScreenEditPage);
  server.on("/screens/save", HTTP_POST, handleScreenSave);
  server.on("/screens/delete", handleScreenDelete);
  server.on("/screens/active", handleScreenSetActive);
  server.on("/config/general", handleGeneralConfig);
  server.on("/config/general/save", HTTP_POST, handleSaveGeneralConfig);
  server.on("/backup", handleBackupRestorePage);
  server.on("/backup/download", handleBackupDownload);
  server.on("/backup/restore", HTTP_POST, handleBackupRestore);
  server.on("/test", handleTestAPI);
  server.on("/reset", handleFactoryReset);
  server.on("/restart", handleRestart);
}

void handleRoot() {
  if (!requireAuth()) return;

  String html = htmlHeader("");
  html += ".info-row{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid #eee}.label{font-weight:bold;color:#666}.value{color:#333}";
  html += "</style></head><body>";
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
  
  // Screens Status
  html += "<h2>Screens Status</h2>";
  if (numScreens == 0) {
    html += "<div class='status warning'>";
    html += "<strong>No Screens Configured</strong><br>";
    html += "Add a screen to begin monitoring. <a href='/screens/edit'>Add Screen</a>";
    html += "</div>";
  } else {
    html += "<div class='status ok'>";
    html += "<strong>Active: " + htmlEscape(screens[activeScreen].name) + " (" + String(activeScreen + 1) + "/" + String(numScreens) + ")</strong><br>";
    html += "Current Value: <strong>" + htmlEscape(screens[activeScreen].currentValue) + "</strong><br>";
    if (screens[activeScreen].lastAPICall > 0) {
      html += "Last Update: " + String((millis() - screens[activeScreen].lastAPICall) / 1000) + "s ago";
    }
    if (screens[activeScreen].lastError.length() > 0) {
      html += "<br>Last Error: " + htmlEscape(screens[activeScreen].lastError);
    }
    html += "</div>";
    if (numScreens > 1) {
      html += "<div style='margin: 10px 0;'>";
      for (int i = 0; i < numScreens; i++) {
        String badgeStyle = (i == activeScreen) ? "background:#4CAF50;color:white;" : "background:#ddd;color:#333;";
        html += "<span style='display:inline-block;padding:4px 10px;margin:2px;border-radius:12px;" + badgeStyle + "font-size:13px;'>";
        html += htmlEscape(screens[i].name) + ": " + htmlEscape(screens[i].currentValue);
        html += "</span>";
      }
      html += "</div>";
      html += "<div class='info-row'><span class='label'>Auto-Rotate:</span><span class='value'>" + String(autoRotate ? "Every " + String(rotateInterval) + "s" : "Off") + "</span></div>";
    }
  }

  // Display Settings
  html += "<h2>Display Settings</h2>";
  html += "<div class='info-row'><span class='label'>Brightness:</span><span class='value'>" + String(autoBrightness ? "Auto" : "Manual (" + String(manualBrightness) + ")") + "</span></div>";

  // Action Buttons
  html += "<h2>Actions</h2>";
  html += "<button onclick='location.href=\"/config/general\"' class='button'>Config</button>";
  html += "<button onclick='location.href=\"/screens\"' class='button'>Manage Screens</button>";
  html += "<button onclick='location.href=\"/backup\"' class='button secondary'>Backup / Restore</button>";
  html += "<button onclick='location.href=\"/status\"' class='button secondary'>View JSON Status</button>";
  html += "<button onclick='if(confirm(\"Restart Device?\")) location.href=\"/restart\"' class='button secondary'>Restart</button>";
  html += "<button onclick='if(confirm(\"Reset all settings?\")) location.href=\"/reset\"' class='button danger'>Factory Reset</button>";
  html += "<button onclick='location.href=\"/logout\"' class='button secondary' style='margin-top: 10px;'>Logout</button>";
  
  html += "</div>";

  // Footer
  html += "<div style='margin-top: 30px; padding-top: 20px; border-top: 1px solid #ddd; text-align: center; color: #666; font-size: 14px;'>";
  html += "<p style='margin: 5px 0;'><strong>Ulanzi TC001 API Monitor</strong> " + buildNumber + "</p>";
  html += "<p style='margin: 5px 0;'>Source Code and Documentation &bull; <a href='https://github.com/TommySharpNZ/Ulanzi-TC001-API-Monitor' target='_blank' style='color: #4CAF50; text-decoration: none;'>GitHub Repository</a></p>";
  html += "<p style='margin: 5px 0; font-size: 12px;'>Built for the community</p>";
  html += "</div>";

  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleGeneralConfig() {
  if (!requireAuth()) return;
  String html = htmlHeader("General Settings");
  html += "</style></head><body>";
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

  // Auto-rotation section
  html += "<h2 style='margin-top: 30px;'>Screen Auto-Rotation</h2>";

  html += "<label><input type='checkbox' name='autoRotate' id='autoRotate' onchange='toggleRotateSettings()' " + String(autoRotate ? "checked" : "") + "><span class='checkbox-label'>Auto-Rotate Screens</span></label>";
  html += "<p class='help'>Automatically cycle through screens at a set interval</p>";

  html += "<div id='rotateGroup' style='display: " + String(autoRotate ? "block" : "none") + ";'>";
  html += "<label>Rotation Interval (seconds):</label>";
  html += "<input type='number' name='rotateInterval' value='" + String(rotateInterval) + "' min='3' max='300'>";
  html += "<p class='help'>How often to switch screens (3-300 seconds)</p>";
  html += "</div>";

  // Admin password section
  html += "<h2 style='margin-top: 30px;'>Admin Password</h2>";
  html += "<label>New Password:</label>";
  html += "<input type='password' name='adminPassword' placeholder='Leave blank to keep current password'>";
  html += "<p class='help'>Change the admin password for accessing the web interface. Leave blank to keep the current password.</p>";

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
  html += "function toggleRotateSettings() {";
  html += "  const isAuto = document.getElementById('autoRotate').checked;";
  html += "  document.getElementById('rotateGroup').style.display = isAuto ? 'block' : 'none';";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveGeneralConfig() {
  if (!requireAuth()) return;
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

  // Auto-rotation settings
  autoRotate = server.hasArg("autoRotate");
  Serial.print("Auto Rotate: ");
  Serial.println(autoRotate ? "ENABLED" : "DISABLED");

  if (server.hasArg("rotateInterval")) {
    rotateInterval = server.arg("rotateInterval").toInt();
    if (rotateInterval < 3) rotateInterval = 3;
    if (rotateInterval > 300) rotateInterval = 300;
    Serial.print("Rotate Interval: ");
    Serial.println(rotateInterval);
  }

  // Check if admin password should be changed
  if (server.hasArg("adminPassword")) {
    String newPassword = server.arg("adminPassword");
    if (newPassword.length() > 0) {
      adminPassword = newPassword;
      Serial.println("Admin password updated");
    }
  }

  // Save to preferences
  Serial.println("Writing to preferences...");
  preferences.begin("tc001", false);
  preferences.putBool("autoBrightness", autoBrightness);
  preferences.putInt("brightness", manualBrightness);
  preferences.putString("adminPassword", adminPassword);
  preferences.putBool("autoRotate", autoRotate);
  preferences.putInt("rotateIntv", rotateInterval);
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

// ============================================
// Screen Management Web Pages
// ============================================

void handleScreensPage() {
  if (!requireAuth()) return;

  String html = htmlHeader("Screens");
  html += ".container{max-width:700px}";
  html += ".screen-card{background:#f9f9f9;border:2px solid #e0e0e0;border-radius:8px;padding:15px;margin:10px 0}";
  html += ".screen-card.active{border-color:#4CAF50;background:#f1f8e9}";
  html += ".screen-header{display:flex;align-items:center;gap:10px;margin-bottom:8px}";
  html += ".screen-number{background:#666;color:#fff;width:28px;height:28px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-weight:bold;font-size:14px}";
  html += ".screen-card.active .screen-number{background:#4CAF50}";
  html += ".screen-name{font-size:18px;font-weight:bold;color:#333}";
  html += ".screen-badge{font-size:11px;padding:2px 8px;border-radius:10px;background:#4CAF50;color:#fff}";
  html += ".screen-detail{color:#666;font-size:13px;margin:4px 0}";
  html += ".screen-value{font-size:16px;font-weight:bold;color:#333;margin:6px 0}";
  html += ".screen-actions{margin-top:10px}";
  html += "</style></head><body>";

  html += "<div class='container'>";
  html += "<h1>Screen Management</h1>";

  html += "<div class='info-box'>";
  html += "Configure up to " + String(MAX_SCREENS) + " screens, each with its own API endpoint. ";
  html += "Use <strong>Button 1</strong> (previous) and <strong>Button 3</strong> (next) on the device to navigate between screens.";
  html += "</div>";

  // Screen list
  if (numScreens == 0) {
    html += "<p style='text-align:center;color:#666;padding:20px;'>No screens configured yet.</p>";
  }

  for (int i = 0; i < numScreens; i++) {
    html += "<div class='screen-card" + String(i == activeScreen ? " active" : "") + "'>";
    html += "<div class='screen-header'>";
    html += "<span class='screen-number'>" + String(i + 1) + "</span>";
    html += "<span class='screen-name'>" + htmlEscape(screens[i].name) + "</span>";
    if (i == activeScreen) {
      html += "<span class='screen-badge'>Active</span>";
    }
    html += "</div>";

    if (screens[i].apiConfigured) {
      html += "<div class='screen-value'>Value: " + htmlEscape(screens[i].currentValue) + "</div>";
      String truncUrl = screens[i].apiEndpoint;
      if (truncUrl.length() > 50) truncUrl = truncUrl.substring(0, 50) + "...";
      html += "<div class='screen-detail'>Endpoint: " + htmlEscape(truncUrl) + "</div>";
      html += "<div class='screen-detail'>Path: " + htmlEscape(screens[i].jsonPath) + " | Interval: " + String(screens[i].pollingInterval) + "s</div>";
    } else {
      html += "<div class='screen-detail' style='color:#f44336;'>Not configured (missing endpoint or JSON path)</div>";
    }

    html += "<div class='screen-actions'>";
    html += "<a href='/screens/edit?id=" + String(i) + "' class='button small'>Edit</a>";
    if (i != activeScreen) {
      html += "<a href='/screens/active?id=" + String(i) + "' class='button small secondary'>Set Active</a>";
    }
    html += "<a href='#' onclick='if(confirm(\"Delete screen: " + htmlEscape(screens[i].name) + "?\")) location.href=\"/screens/delete?id=" + String(i) + "\"' class='button small danger'>Delete</a>";
    html += "</div>";
    html += "</div>";
  }

  // Add screen button
  if (numScreens < MAX_SCREENS) {
    html += "<div style='text-align:center;margin:20px 0;'>";
    html += "<a href='/screens/edit' class='button'>+ Add Screen</a>";
    html += "</div>";
  } else {
    html += "<p style='text-align:center;color:#666;'>Maximum " + String(MAX_SCREENS) + " screens reached.</p>";
  }

  html += "<div style='margin-top:20px;'>";
  html += "<a href='/' class='button secondary'>Back to Home</a>";
  html += "</div>";

  html += "</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleScreenEditPage() {
  if (!requireAuth()) return;

  int screenIdx = -1;
  bool isNew = true;
  String screenName = "Screen " + String(numScreens + 1);
  String scrEndpoint = "";
  String scrKey = "";
  String scrHeader = "APIKey";
  String scrJsonPath = "";
  String scrPrefix = "";
  String scrSuffix = "";
  int scrInterval = 60;
  bool scrScroll = true;
  String scrIconData = "";

  if (server.hasArg("id")) {
    screenIdx = server.arg("id").toInt();
    if (screenIdx >= 0 && screenIdx < numScreens) {
      isNew = false;
      Screen& scr = screens[screenIdx];
      screenName = scr.name;
      scrEndpoint = scr.apiEndpoint;
      scrKey = scr.apiKey;
      scrHeader = scr.apiHeaderName;
      scrJsonPath = scr.jsonPath;
      scrPrefix = scr.displayPrefix;
      scrSuffix = scr.displaySuffix;
      scrInterval = scr.pollingInterval;
      scrScroll = scr.scrollEnabled;
      scrIconData = scr.iconData;
    } else {
      screenIdx = -1;
    }
  }

  String maskedKey = "";
  if (scrKey.length() > 0) {
    for (unsigned int i = 0; i < scrKey.length(); i++) maskedKey += "*";
  }

  String pageTitle = isNew ? "Add Screen" : "Edit Screen: " + screenName;

  String html = htmlHeader(pageTitle);
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>" + pageTitle + "</h1>";

  html += "<form method='POST' action='/screens/save' accept-charset='utf-8'>";
  html += "<input type='hidden' name='id' value='" + String(isNew ? -1 : screenIdx) + "'>";

  html += "<label>Screen Name:</label>";
  html += "<input type='text' name='name' value='" + htmlEscape(screenName) + "' placeholder='My Dashboard' required>";
  html += "<p class='help'>A friendly name for this screen (shown when switching)</p>";

  html += "<label>API Endpoint URL:</label>";
  html += "<input type='text' name='apiUrl' value='" + htmlEscape(scrEndpoint) + "' placeholder='https://api.example.com/data' required>";
  html += "<p class='help'>Full URL to your API endpoint</p>";

  html += "<label>API Header Name:</label>";
  html += "<input type='text' name='apiHeader' value='" + htmlEscape(scrHeader) + "' placeholder='APIKey'>";
  html += "<p class='help'>Authentication header name (e.g., APIKey, Authorization, X-API-Key)</p>";

  html += "<label>API Key:</label>";
  html += "<input type='password' name='apiKey' value='" + htmlEscape(scrKey) + "' placeholder='" + (scrKey.length() > 0 ? maskedKey : "your-api-key-here") + "'>";
  html += "<p class='help'>Your API authentication key</p>";

  html += "<label>JSON Path:</label>";
  html += "<input type='text' name='jsonPath' value='" + htmlEscape(scrJsonPath) + "' placeholder='data.count' required>";
  html += "<p class='help'>Path to value in JSON response (e.g., data.count or users[0].name)</p>";

  html += "<label>Display Prefix:</label>";
  html += "<input type='text' name='prefix' value='" + htmlEscape(scrPrefix) + "' placeholder='Tickets: '>";
  html += "<p class='help'>Text to show before the value (optional)</p>";

  html += "<label>Display Suffix:</label>";
  html += "<input type='text' name='suffix' value='" + htmlEscape(scrSuffix) + "' placeholder=' open'>";
  html += "<p class='help'>Text to show after the value (optional)</p>";

  html += "<label>Icon Data (8x8 RGB pixels):</label>";
  html += "<textarea name='iconData' id='iconData' placeholder='[[255,0,0],[0,255,0],...]'>" + htmlEscape(scrIconData) + "</textarea>";
  html += "<p class='help'>JSON array of 64 RGB pixels [r,g,b] for 8x8 icon (optional)</p>";
  html += "<div id='iconPreview' class='icon-preview'></div>";

  html += "<label><input type='checkbox' name='scroll' " + String(scrScroll ? "checked" : "") + "><span class='checkbox-label'>Enable Scrolling</span></label>";
  html += "<p class='help'>Uncheck for static centered display</p>";

  html += "<label>Polling Interval (seconds):</label>";
  html += "<input type='number' name='interval' value='" + String(scrInterval) + "' min='5' max='3600' required>";
  html += "<p class='help'>How often to poll this screen's API (5-3600 seconds)</p>";

  html += "<button type='submit' class='button'>Save Screen</button>";
  if (!isNew) {
    html += "<button type='button' class='button secondary' onclick='testAPI()'>Test Connection</button>";
  }
  html += "<button type='button' onclick='location.href=\"/screens\"' class='button secondary'>Back</button>";

  html += "</form>";

  html += "<div id='testResult' style='margin-top: 20px;'></div>";
  html += "</div>";

  // JavaScript
  html += "<script>";
  if (!isNew) {
    html += "function testAPI() {";
    html += "  document.getElementById('testResult').innerHTML = '<p>Testing connection...</p>';";
    html += "  fetch('/test?screen=" + String(screenIdx) + "').then(r => r.text()).then(data => {";
    html += "    document.getElementById('testResult').innerHTML = '<div class=\"status ok\"><pre>' + data + '</pre></div>';";
    html += "  }).catch(err => {";
    html += "    document.getElementById('testResult').innerHTML = '<div class=\"status error\">Error: ' + err + '</div>';";
    html += "  });";
    html += "}";
  }

  // Icon editor JavaScript (same as before)
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
  html += "  return result ? [parseInt(result[1], 16), parseInt(result[2], 16), parseInt(result[3], 16)] : [0, 0, 0];";
  html += "}";

  html += "function updatePreviewFromJSON() {";
  html += "  if (isUpdatingFromPreview) return;";
  html += "  try {";
  html += "    const data = JSON.parse(document.getElementById('iconData').value);";
  html += "    if (Array.isArray(data) && data.length === 64) { iconDataArray = data; renderPreview(); }";
  html += "    else { document.getElementById('iconPreview').innerHTML = '<p style=\"color:red\">Invalid: Need exactly 64 pixels</p>'; }";
  html += "  } catch(e) {";
  html += "    if (document.getElementById('iconData').value.trim() === '') { iconDataArray = Array(64).fill([0,0,0]); renderPreview(); }";
  html += "    else { document.getElementById('iconPreview').innerHTML = ''; }";
  html += "  }";
  html += "}";

  html += "function updateJSONFromPreview() {";
  html += "  isUpdatingFromPreview = true;";
  html += "  document.getElementById('iconData').value = JSON.stringify(iconDataArray);";
  html += "  setTimeout(() => { isUpdatingFromPreview = false; }, 10);";
  html += "}";

  html += "function updateColorDisplay() {";
  html += "  const el = document.getElementById('rgbDisplay');";
  html += "  if (el) el.textContent = 'RGB: ' + currentColor.join(', ');";
  html += "}";

  html += "function renderPreview() {";
  html += "  let p = '<div style=\"margin-bottom:10px;\">';";
  html += "  p += '<label>Color Picker: </label>';";
  html += "  p += '<input type=\"color\" id=\"colorPicker\" value=\"' + rgbToHex(currentColor[0],currentColor[1],currentColor[2]) + '\" style=\"width:60px;height:30px;border:none;cursor:pointer;\">';";
  html += "  p += '<span id=\"rgbDisplay\" style=\"margin-left:10px;\">RGB: ' + currentColor.join(', ') + '</span>';";
  html += "  p += '<button type=\"button\" onclick=\"clearIcon()\" style=\"margin-left:15px;padding:5px 10px;cursor:pointer;\">Clear All</button>';";
  html += "  p += '</div><div id=\"pixelGrid\" style=\"display:inline-block;border:2px solid #ccc;\">';";
  html += "  for (let i = 0; i < 8; i++) { for (let j = 0; j < 8; j++) {";
  html += "    const idx = i*8+j; const px = iconDataArray[idx]||[0,0,0];";
  html += "    p += '<div class=\"icon-pixel\" id=\"pixel'+idx+'\" onclick=\"paintPixel('+idx+')\" style=\"background-color:rgb('+px[0]+','+px[1]+','+px[2]+');cursor:pointer;\" title=\"Click to paint\"></div>';";
  html += "  } p += '<br>'; }";
  html += "  p += '</div>';";
  html += "  document.getElementById('iconPreview').innerHTML = p;";
  html += "  if (!colorPickerInitialized) { setupColorPicker(); colorPickerInitialized = true; }";
  html += "}";

  html += "function setupColorPicker() {";
  html += "  const cp = document.getElementById('colorPicker');";
  html += "  if (cp) cp.addEventListener('input', function(e) { currentColor = hexToRgb(e.target.value); updateColorDisplay(); });";
  html += "}";

  html += "function paintPixel(index) {";
  html += "  iconDataArray[index] = [...currentColor];";
  html += "  const el = document.getElementById('pixel' + index);";
  html += "  if (el) el.style.backgroundColor = 'rgb(' + currentColor[0] + ',' + currentColor[1] + ',' + currentColor[2] + ')';";
  html += "  updateJSONFromPreview();";
  html += "}";

  html += "function clearIcon() {";
  html += "  iconDataArray = Array(64).fill([0,0,0]);";
  html += "  for (let i = 0; i < 64; i++) { const el = document.getElementById('pixel'+i); if (el) el.style.backgroundColor = 'rgb(0,0,0)'; }";
  html += "  updateJSONFromPreview();";
  html += "}";

  html += "document.getElementById('iconData').addEventListener('input', updatePreviewFromJSON);";
  html += "window.addEventListener('load', function() { updatePreviewFromJSON(); });";

  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleScreenSave() {
  if (!requireAuth()) return;

  int screenIdx = server.arg("id").toInt();
  bool isNew = (screenIdx < 0 || screenIdx >= numScreens);

  if (isNew) {
    if (numScreens >= MAX_SCREENS) {
      server.send(400, "text/plain", "Maximum screens reached");
      return;
    }
    screenIdx = numScreens;
    numScreens++;
  }

  Screen& scr = screens[screenIdx];
  scr.name = server.arg("name");
  scr.apiEndpoint = server.arg("apiUrl");
  scr.apiHeaderName = server.arg("apiHeader");
  scr.apiKey = server.arg("apiKey");
  scr.jsonPath = server.arg("jsonPath");
  scr.displayPrefix = server.arg("prefix");
  scr.displaySuffix = server.arg("suffix");
  scr.pollingInterval = server.arg("interval").toInt();
  scr.scrollEnabled = server.hasArg("scroll");
  scr.iconData = server.arg("iconData");

  if (scr.pollingInterval < 5) scr.pollingInterval = 5;
  if (scr.pollingInterval > 3600) scr.pollingInterval = 3600;

  if (scr.iconData.length() > 0) {
    parseIconData(scr.iconData, scr.iconPixels, scr.iconEnabled);
  } else {
    scr.iconEnabled = false;
  }

  scr.apiConfigured = (scr.apiEndpoint.length() > 0 && scr.jsonPath.length() > 0);

  // Initialize runtime state for new screens
  if (isNew) {
    scr.currentValue = "---";
    scr.lastError = "";
    scr.lastAPICall = 0;
  }

  // Save to NVS
  preferences.begin("tc001", false);
  preferences.putInt("numScreens", numScreens);
  saveScreenToPrefs(screenIdx);
  preferences.end();

  Serial.println("Screen " + String(screenIdx) + " saved: " + scr.name);

  // Poll immediately if configured
  if (scr.apiConfigured) {
    pollScreenAPI(screenIdx);
    scr.lastAPICall = millis();
  }

  // If this is the first screen, make it active
  if (numScreens == 1) {
    activeScreen = 0;
    preferences.begin("tc001", false);
    preferences.putInt("activeScr", activeScreen);
    preferences.end();
  }

  scrollX = MATRIX_WIDTH;

  // Redirect to screens page
  server.sendHeader("Location", "/screens");
  server.send(303);
}

void handleScreenDelete() {
  if (!requireAuth()) return;

  if (!server.hasArg("id")) {
    server.sendHeader("Location", "/screens");
    server.send(303);
    return;
  }

  int screenIdx = server.arg("id").toInt();
  if (screenIdx < 0 || screenIdx >= numScreens) {
    server.sendHeader("Location", "/screens");
    server.send(303);
    return;
  }

  Serial.println("Deleting screen " + String(screenIdx) + ": " + screens[screenIdx].name);

  preferences.begin("tc001", false);

  // Remove last screen's NVS keys (we'll shift data down and re-save)
  removeScreenFromPrefs(numScreens - 1);

  // Shift screens down in memory
  for (int i = screenIdx; i < numScreens - 1; i++) {
    screens[i] = screens[i + 1];
  }

  numScreens--;

  // Adjust active screen
  if (numScreens == 0) {
    activeScreen = 0;
  } else if (activeScreen >= numScreens) {
    activeScreen = numScreens - 1;
  } else if (activeScreen > screenIdx) {
    activeScreen--;
  }

  // Re-save all screens in new order
  preferences.putInt("numScreens", numScreens);
  preferences.putInt("activeScr", activeScreen);
  for (int i = 0; i < numScreens; i++) {
    saveScreenToPrefs(i);
  }

  preferences.end();

  scrollX = MATRIX_WIDTH;

  server.sendHeader("Location", "/screens");
  server.send(303);
}

void handleScreenSetActive() {
  if (!requireAuth()) return;

  if (server.hasArg("id")) {
    int screenIdx = server.arg("id").toInt();
    if (screenIdx >= 0 && screenIdx < numScreens) {
      switchToScreen(screenIdx);
    }
  }

  server.sendHeader("Location", "/screens");
  server.send(303);
}

void handleTestAPI() {
  if (!requireAuth()) return;

  int screenIdx = 0;
  if (server.hasArg("screen")) {
    screenIdx = server.arg("screen").toInt();
  }

  if (screenIdx < 0 || screenIdx >= numScreens) {
    server.send(400, "text/plain", "Error: Invalid screen index");
    return;
  }

  Screen& scr = screens[screenIdx];

  if (scr.apiEndpoint.length() == 0) {
    server.send(400, "text/plain", "Error: No API endpoint configured for this screen");
    return;
  }

  const int TIMEOUT_MS = 10000;

  Serial.println("[Test Screen " + String(screenIdx) + "] Testing API: " + scr.apiEndpoint);

  String encodedUrl = urlEncode(scr.apiEndpoint);

  HTTPClient http;
  http.begin(encodedUrl);
  http.setTimeout(TIMEOUT_MS);

  if (scr.apiKey.length() > 0) {
    http.addHeader(scr.apiHeaderName, scr.apiKey);
    Serial.println("Added header: " + scr.apiHeaderName);
  }

  int httpCode = http.GET();

  String response = "Screen: " + scr.name + "\n";
  response += "API Endpoint: " + scr.apiEndpoint + "\n";
  if (encodedUrl != scr.apiEndpoint) {
    response += "Encoded URL: " + encodedUrl + "\n";
  }
  response += "\nHTTP Code: " + String(httpCode) + "\n\n";

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      response += "Response:\n" + payload + "\n\n";

      String extractedValue = extractJSONValue(payload, scr.jsonPath);
      if (extractedValue.length() > 0) {
        response += "Extracted Value: " + extractedValue;
      } else {
        response += "Error: Could not extract value from JSON path: " + scr.jsonPath;
      }
    } else {
      response += "Error: " + http.getString();
    }
  } else {
    response += "Error: " + http.errorToString(httpCode);
  }

  http.end();

  server.send(200, "text/plain", response);
  Serial.println("Test API response sent");
}

void handleRestart() {
  if (!requireAuth()) return;
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
  if (!requireAuth()) return;
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
  DynamicJsonDocument doc(4096);

  doc["device"] = deviceName;
  doc["device_id"] = deviceID;
  doc["ip"] = ipAddress;
  doc["ssid"] = WiFi.SSID();
  doc["rssi"] = WiFi.RSSI();
  doc["num_screens"] = numScreens;
  doc["active_screen"] = activeScreen;
  doc["auto_rotate"] = autoRotate;
  doc["rotate_interval"] = rotateInterval;

  JsonArray screensArr = doc.createNestedArray("screens");
  for (int i = 0; i < numScreens; i++) {
    JsonObject s = screensArr.createNestedObject();
    s["name"] = screens[i].name;
    s["api_configured"] = screens[i].apiConfigured;
    s["current_value"] = screens[i].currentValue;
    s["last_error"] = screens[i].lastError;
    s["icon_enabled"] = screens[i].iconEnabled;
  }

  doc["battery_voltage"] = serialized(String(batteryVoltage, 2));
  doc["battery_percentage"] = batteryPercentage;

  String json;
  serializeJson(doc, json);
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
