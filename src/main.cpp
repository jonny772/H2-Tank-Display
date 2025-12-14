#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include "display/Arduino_AXS15231B.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <Wire.h>
#include <vector>
#include <algorithm>

// Wi-Fi credentials - ONLY 1 DEFAULT OTHERS ADDED BY UI. Will persist across flashes
constexpr char WIFI_SSID[] = "LUinc-Members";
constexpr char WIFI_PASSWORD[] = "eFGEpC-uH2";
//constexpr char WIFI_SSID[] = "iPhone";
//constexpr char WIFI_PASSWORD[] = "tttttttt";

// Arduino Cloud OAuth2 credentials
constexpr char CLIENT_ID[] = "TMi3JA5FjHcMhz1Ib2pzrg9uZRTpYMoM";
constexpr char CLIENT_SECRET[] = "LZcpGsJVFApJV3dpv5zjVeVHlkVI43qQK9yoyfP4PkW78pSF4sAjogPfpE2OTwT5";

// Thing and property names
constexpr char THING_NAME[] = "MESCH_M_IOT_R1";
constexpr char PROPERTY_NAME[] = "aI_in__Pres_PO_H2_out";
constexpr char PROPERTY_VAR_NAME[] = "aI_in__Pres_PO_H2_out";
// Direct lookup IDs (avoids huge payloads)
constexpr char THING_ID[] = "9ff4611c-bd42-4c69-86b4-9245cfb82037";
constexpr char PROPERTY_ID[] = "26ac0e3f-49cf-484d-9f08-ca02a8c49698";

// Pressure scaling
constexpr float MAX_BAR_VALUE = 1.5f;  // 100%

// Battery monitor pins (board variants show BAT_VOLT on GPIO2; some defs use GPIO8). Read both.
constexpr int BATTERY_ADC_PIN_PRIMARY = 2;
constexpr int BATTERY_ADC_PIN_ALT = 8;
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;  // voltage divider halves battery voltage
constexpr float BATTERY_MIN_V = 3.3f;
constexpr float BATTERY_MAX_V = 4.0f;

constexpr uint16_t HTTP_TIMEOUT_MS = 5000;

// Color aliases
constexpr uint16_t COLOR_BLACK = RGB565_BLACK;
constexpr uint16_t COLOR_WHITE = RGB565_WHITE;
constexpr uint16_t COLOR_GREEN = RGB565_GREEN;
constexpr uint16_t COLOR_YELLOW = RGB565_YELLOW;
constexpr uint16_t COLOR_DARKGREY = 0x8410;  // mid grey
constexpr uint16_t COLOR_LIGHTBLUE = RGB565_CYAN;
constexpr uint16_t COLOR_RED = RGB565_RED;

// Display pins for LilyGO T-Display S3 Long
constexpr int TFT_QSPI_CS = 12;
constexpr int TFT_QSPI_SCK = 17;
constexpr int TFT_QSPI_D0 = 13;
constexpr int TFT_QSPI_D1 = 18;
constexpr int TFT_QSPI_D2 = 21;
constexpr int TFT_QSPI_D3 = 14;
constexpr int TFT_QSPI_RST = 16;
constexpr int TFT_BL = 1;

constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 1;

// Touch (AXS15231B) pins
constexpr int TOUCH_IICSCL = 10;
constexpr int TOUCH_IICSDA = 15;
constexpr int TOUCH_INT = 11;
constexpr int TOUCH_RES = 16;

// Touch controller
constexpr uint8_t TOUCH_ADDR = 0x3B;
constexpr uint8_t TOUCH_CMD[] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
volatile bool g_touchInterrupt = false;
unsigned long nextTouchPollMs = 0;
uint8_t lastTouchBuf[8] = {0};
bool loggedTouchPresence = false;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_QSPI_CS, TFT_QSPI_SCK, TFT_QSPI_D0, TFT_QSPI_D1, TFT_QSPI_D2, TFT_QSPI_D3);
// Use the 180x640 init sequence (default) with rotation 0
Arduino_GFX *gfx = new Arduino_AXS15231B(bus, TFT_QSPI_RST, 0 /* rotation */,
                                         false /* IPS */, 180, 640);

int screenW = 0;
int screenH = 0;
WiFiClientSecure secureClient;
Preferences prefs;
std::vector<std::pair<String, String>> knownNetworks;
size_t currentNetworkIndex = 0;
unsigned long lastConnectivityCheck = 0;
const unsigned long connectivityIntervalMs = 30000;
String accessToken;
unsigned long tokenExpiresAt = 0;  // millis when token expires

int16_t lastTouchX = -1;
int16_t lastTouchY = -1;
float lastPressureReading = 0.0f;
const unsigned long dataFetchIntervalMs = 30000;  // 30s polling
unsigned long lastDataFetchMs = 0;
unsigned long lastStatusDrawMs = 0;
const unsigned long statusDrawIntervalMs = 2000;  // redraw top bar every 2 seconds
const unsigned long criticalBlinkIntervalMs = 500;  // bar blink cadence for <10%
unsigned long lastWifiTapMs = 0;
const unsigned long doubleTapWindowMs = 500;  // ms between taps to open WiFi
constexpr bool SERIAL_VERBOSE = false;
bool wifiSettingsActive = false;
size_t lastWorkingNetworkIndex = 0;
bool hasLastWorkingNetwork = false;
bool awaitingNewNetworkToken = false;
unsigned long newNetworkStartMs = 0;
unsigned long lastBatterySampleMs = 0;
const unsigned long batterySampleIntervalMs = 2000;  // read battery every 2s
float batteryAvgVoltage = 0.0f;
float cachedBatteryPercent = 0.0f;
unsigned long lastWifiAttemptMs = 0;
const unsigned long wifiReconnectIntervalMs = 15000;
unsigned long lastBarBlinkMs = 0;
unsigned long lastDataUpdateMs = 0;
const unsigned long dataStaleMs = 5UL * 60UL * 1000UL;
bool dataOldNotified = false;

struct TouchEvent {
  int16_t x;
  int16_t y;
  uint8_t fingers;
  uint8_t event;
};

QueueHandle_t touchQueue = nullptr;

void connectWiFi();
bool refreshAccessToken();
float fetchPressure();
void drawScaffold();
void drawBar(float valueBar);
void showStatus(const String &msg);
void drawStatusIcons();
void handleSerialCommands();
float readBatteryPercent();
int wifiBars();
bool hasInternetConnectivity();
void loadSavedNetworks();
void saveNetworks();
void ensureNetwork(const String &ssid, const String &password);
int findNetworkIndex(const String &ssid);
bool readTouch(int16_t &x, int16_t &y);
bool readTouch(int16_t &x, int16_t &y, uint8_t &fingers, uint8_t &touchEvent);
bool isWifiIconTouched(int16_t x, int16_t y);
void openWiFiSettings();
int selectNetworkFromList(const std::vector<String> &ssids, int startY, int rowHeight);
String promptForPassword(const String &ssid);
std::vector<String> scanUniqueNetworks();
void drawNetworkList(const std::vector<String> &ssids, int startIndex, int startY, int rowHeight, int visibleRows);
void clearTouchQueue();
void scanI2CBus();
void printBuf(const uint8_t *buf, size_t len);
void touchTask(void *param);

void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  ledcAttachPin(TFT_BL, BACKLIGHT_PWM_CHANNEL);
  ledcSetup(BACKLIGHT_PWM_CHANNEL, 2000, 8);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);  // keep off until the panel is ready

  analogSetPinAttenuation(BATTERY_ADC_PIN_PRIMARY, ADC_11db);
  analogSetPinAttenuation(BATTERY_ADC_PIN_ALT, ADC_11db);
  cachedBatteryPercent = readBatteryPercent();

  // Touch
  pinMode(TOUCH_RES, OUTPUT);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(TOUCH_INT, [] { g_touchInterrupt = true; }, FALLING);

  // Reset touch controller (AXS15231B sequence from LilyGO example)
  digitalWrite(TOUCH_RES, HIGH);
  delay(2);
  digitalWrite(TOUCH_RES, LOW);
  delay(100);
  digitalWrite(TOUCH_RES, HIGH);
  delay(2);

  Wire.begin(TOUCH_IICSDA, TOUCH_IICSCL, 400000);
  // Quick scan to confirm touch is present on the expected address.
  Wire.beginTransmission(TOUCH_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("Touch controller not detected on 0x3B");
  }
  scanI2CBus();

  gfx->begin();
  screenW = gfx->width();
  screenH = gfx->height();
  gfx->fillScreen(COLOR_BLACK);
  gfx->setTextColor(COLOR_WHITE, COLOR_BLACK);
  gfx->setTextSize(2);

  prefs.begin("wifi", false);
  loadSavedNetworks();
  ensureNetwork(WIFI_SSID, WIFI_PASSWORD);

  drawScaffold();
  showStatus("WiFi...");

  connectWiFi();
  drawStatusIcons();

  secureClient.setInsecure();
  if (WiFi.status() == WL_CONNECTED) {
    showStatus("Auth...");
    refreshAccessToken();
  } else {
    showStatus("No WiFi");
  }

  // Spawn touch task on core 0 to keep UI loop responsive on core 1.
  touchQueue = xQueueCreate(8, sizeof(TouchEvent));
  xTaskCreatePinnedToCore(touchTask, "touchTask", 4096, nullptr, 1, nullptr, 0);

  // Fade backlight up
  for (int i = 0; i <= 255; i += 8) {
    ledcWrite(BACKLIGHT_PWM_CHANNEL, i);
    delay(5);
  }
}

void loop() {
  TouchEvent evt;
  while (touchQueue && xQueueReceive(touchQueue, &evt, 0) == pdTRUE) {
    lastTouchX = evt.x;
    lastTouchY = evt.y;
    bool inWifiIcon = isWifiIconTouched(evt.x, evt.y);
    // Double-tap on Wi-Fi icon to open settings.
    if (inWifiIcon && (evt.event == 0 || evt.event == 1)) {  // down or up counts as a tap
      unsigned long now = millis();
      if (now - lastWifiTapMs <= doubleTapWindowMs) {
        openWiFiSettings();
        lastWifiTapMs = 0;
      } else {
        lastWifiTapMs = now;
      }
    }
  }

  handleSerialCommands();

  bool wifiOk = WiFi.status() == WL_CONNECTED;

  if (!wifiOk) {
    unsigned long now = millis();
    if (!wifiSettingsActive && (now - lastWifiAttemptMs > wifiReconnectIntervalMs)) {
      showStatus("Reconnect WiFi");
      lastWifiAttemptMs = now;
      connectWiFi();
    }
  }

  if (wifiOk && millis() - lastConnectivityCheck > connectivityIntervalMs) {
    lastConnectivityCheck = millis();
    if (!hasInternetConnectivity() && !knownNetworks.empty()) {
      showStatus("Find internet");
      currentNetworkIndex = (currentNetworkIndex + 1) % knownNetworks.size();
      connectWiFi();
    }
  }

  if (wifiOk && millis() > tokenExpiresAt) {
    showStatus("Refresh token");
    if (!refreshAccessToken()) {
      delay(5000);
      return;
    }
  }

  if (wifiOk && millis() - lastDataFetchMs >= dataFetchIntervalMs) {
    lastDataFetchMs = millis();
    float pressure = fetchPressure();
    if (pressure >= 0.0f) {
      if (lastPressureReading <= 0.001f) {
        lastPressureReading = pressure;  // first valid sample, no smoothing
      } else {
        // Low-pass filter to avoid jittering display when readings fluctuate
        lastPressureReading = (lastPressureReading * 0.8f) + (pressure * 0.2f);
      }
      lastDataUpdateMs = millis();
      dataOldNotified = false;
      drawBar(lastPressureReading);
    }
  }

  unsigned long now = millis();
  if (now - lastStatusDrawMs >= statusDrawIntervalMs) {
    lastStatusDrawMs = now;
    if (now - lastBatterySampleMs >= batterySampleIntervalMs) {
      // Touch battery only when we are about to draw status to keep load low.
      cachedBatteryPercent = readBatteryPercent();
      lastBatterySampleMs = now;
    }
    drawStatusIcons();
  }

  if (lastDataUpdateMs > 0 && (now - lastDataUpdateMs > dataStaleMs) &&
      !dataOldNotified) {
    showStatus("Old data");
    dataOldNotified = true;
  }

  // If we tried a new network and failed to get a token within 60s, revert to last working.
  if (awaitingNewNetworkToken && (millis() - newNetworkStartMs > 60000)) {
    awaitingNewNetworkToken = false;
    if (hasLastWorkingNetwork && lastWorkingNetworkIndex < knownNetworks.size()) {
      currentNetworkIndex = lastWorkingNetworkIndex;
      showStatus("Revert WiFi");
      accessToken = "";
      tokenExpiresAt = 0;
      connectWiFi();
    }
  }

  // Force redraws when in critical range so the bar actually flashes.
  float percent = (lastPressureReading / MAX_BAR_VALUE) * 100.0f;
  if (percent < 10.0f) {
    unsigned long nowBlink = millis();
    if (nowBlink - lastBarBlinkMs >= criticalBlinkIntervalMs) {
      lastBarBlinkMs = nowBlink;
      drawBar(lastPressureReading);
    }
  }

  delay(10);
}

void connectWiFi() {
  if (knownNetworks.empty()) {
    ensureNetwork(WIFI_SSID, WIFI_PASSWORD);
  }
  lastWifiAttemptMs = millis();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  for (size_t i = 0; i < knownNetworks.size(); i++) {
    size_t attemptIndex = (currentNetworkIndex + i) % knownNetworks.size();
    const auto &cred = knownNetworks[attemptIndex];
    if (cred.first.isEmpty()) continue;

    showStatus(String("Join ") + cred.first);
    WiFi.begin(cred.first.c_str(), cred.second.c_str());
    int attempts = 0;
    const int maxAttempts = 40;  // ~6s per network for more reliable association
    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
      delay(150);
      attempts++;
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (hasInternetConnectivity()) {
        currentNetworkIndex = attemptIndex;
        showStatus("WiFi OK");
        drawStatusIcons();
        return;
      }
      WiFi.disconnect(true);
      delay(200);
    }
  }

  showStatus("WiFi fail");
}

bool refreshAccessToken() {
  HTTPClient http;
  const char *tokenUrl = "https://api2.arduino.cc/iot/v1/clients/token";

  http.begin(secureClient, tokenUrl);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(HTTP_TIMEOUT_MS);

  String body = String("grant_type=client_credentials&client_id=") + CLIENT_ID +
                "&client_secret=" + CLIENT_SECRET +
                "&audience=https%3A%2F%2Fapi2.arduino.cc%2Fiot";
  int status = http.POST(body);
  if (status != HTTP_CODE_OK) {
    showStatus("Token err");
    Serial.printf("Token request failed: %d\n", status);
    String resp = http.getString();
    Serial.println(resp);
    http.end();
    return false;
  }

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err) {
    showStatus("Token parse");
    Serial.printf("Token parse error: %s\n", err.c_str());
    http.end();
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"].as<int>();
  tokenExpiresAt = millis() + (expiresIn - 30) * 1000UL;  // refresh 30s early
  showStatus("Token OK");
  hasLastWorkingNetwork = true;
  lastWorkingNetworkIndex = currentNetworkIndex;
  awaitingNewNetworkToken = false;

  http.end();
  return true;
}

float fetchPressure() {
  if (accessToken.isEmpty()) {
    if (!refreshAccessToken()) {
      return -1.0f;
    }
  }

  HTTPClient http;
  // Single property endpoint only; avoids massive payloads
  if (strlen(THING_ID) == 0 || strlen(PROPERTY_ID) == 0) {
    showStatus("IDs missing");
    return -1.0f;
  }

  String url = String("https://api2.arduino.cc/iot/v2/things/") + THING_ID + "/properties/" + PROPERTY_ID;
  http.begin(secureClient, url);
  http.addHeader("Authorization", String("Bearer ") + accessToken);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");  // avoid gzip
  http.setTimeout(HTTP_TIMEOUT_MS);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    showStatus("Data err");
    Serial.printf("Property request failed: %d\n", status);
    String resp = http.getString();
    if (resp.length()) {
      Serial.println(resp);
    }
    http.end();
    return -1.0f;
  }

  StaticJsonDocument<256> filter;
  filter["name"] = true;
  filter["variable_name"] = true;
  filter["last_value"] = true;

  StaticJsonDocument<768> doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  if (err) {
    showStatus("Data parse");
    Serial.printf("Property parse error: %s\n", err.c_str());
    return -1.0f;
  }

  float value = -1.0f;
  if (doc["last_value"].is<const char *>()) {
    value = atof(doc["last_value"].as<const char *>());
  } else {
    value = doc["last_value"].as<float>();
  }

  http.end();

  if (value < 0.0f) {
    showStatus("No data");
    Serial.println("Parsed value missing from property response");
    return -1.0f;
  }

  return value;
}

void drawScaffold() {
  gfx->fillScreen(COLOR_BLACK);

  gfx->fillRect(0, 0, screenW, 50, COLOR_BLACK);
  drawStatusIcons();
  gfx->setCursor((screenW / 2) - 50, 60);
  gfx->print("H2 Tank");
  gfx->setCursor(10, 88);
  gfx->print("Level (0-100%)");
  drawBar(lastPressureReading);
}

void drawBar(float valueBar) {
  // Vertical bar with comfortable side margins to avoid edge artifacts
  int barTop = 120;
  int barHeight = screenH - barTop - 60;
  int barLeft = 12;
  int barWidth = screenW - (barLeft * 2);
  if (barHeight < 40) barHeight = 40;
  if (barWidth < 20) barWidth = 20;

  float clamped = valueBar;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > MAX_BAR_VALUE) clamped = MAX_BAR_VALUE;

  float percent = (clamped / MAX_BAR_VALUE) * 100.0f;
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  int filled = static_cast<int>(barHeight * (percent / 100.0f));

  // Choose color based on level and blink if critical
  bool dataIsOld = lastDataUpdateMs > 0 && (millis() - lastDataUpdateMs > dataStaleMs);
  bool criticalBlink = !dataIsOld && percent < 10.0f;
  bool blinkOn = ((millis() / 500) % 2) == 0;  // 1Hz blink
  uint16_t fillColor = (percent < 20.0f) ? COLOR_RED : COLOR_LIGHTBLUE;
  if (dataIsOld) {
    fillColor = COLOR_DARKGREY;
  } else if (criticalBlink && !blinkOn) {
    fillColor = COLOR_BLACK;
  }

  // Clear full gauge band to prevent edge pixelation from prior draws
  gfx->fillRect(0, barTop, screenW, barHeight, COLOR_BLACK);

  gfx->drawRect(barLeft, barTop, barWidth, barHeight, COLOR_WHITE);
  // Graduations at 0%, 25%, 50%, 75%, 100%
  for (int i = 0; i <= 4; i++) {
    int y = barTop + (barHeight * i) / 4;
    gfx->drawFastHLine(barLeft - 4, y, 6, COLOR_WHITE);
    gfx->drawFastHLine(barLeft + barWidth - 2, y, 6, COLOR_WHITE);
  }
  gfx->drawRect(barLeft, barTop, barWidth, barHeight, COLOR_WHITE);
  if (filled > 0) {
    int fillTop = barTop + (barHeight - filled);
    gfx->fillRect(barLeft + 1, fillTop + 1, barWidth - 2, filled - 2, fillColor);
    // Sharp transition line at fill top
    gfx->drawFastHLine(barLeft + 1, fillTop, barWidth - 2, COLOR_WHITE);
  }

  String percentStr = String(static_cast<int>(percent + 0.5f)) + "%";
  int16_t tbx, tby;
  uint16_t tbw, tbh;
  gfx->setTextSize(3);
  gfx->getTextBounds(percentStr.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
  int labelX = barLeft + (barWidth - tbw) / 2;
  int labelY = barTop + (barHeight - tbh) / 2;
  int padding = 4;
  gfx->fillRect(labelX - padding, labelY - padding, tbw + padding * 2,
                tbh + padding * 2, COLOR_BLACK);
  gfx->setCursor(labelX, labelY);
  gfx->print(percentStr);
  gfx->setTextSize(2);
}

void drawStatusIcons() {
  static int lastWifiBars = -1;
  static String lastSsid = "";
  static int lastBatteryPercent = -1;

  bool wifiConnected = WiFi.status() == WL_CONNECTED;
  String ssid = wifiConnected ? WiFi.SSID() : "No WiFi";
  int bars = wifiConnected ? wifiBars() : 0;
  int batteryPercent = static_cast<int>(cachedBatteryPercent + 0.5f);

  if (bars == lastWifiBars && ssid == lastSsid && batteryPercent == lastBatteryPercent) {
    return;  // Nothing to redraw; avoid extra fillRects on the constrained ESP32S3.
  }

  lastWifiBars = bars;
  lastSsid = ssid;
  lastBatteryPercent = batteryPercent;

  gfx->fillRect(0, 0, screenW, 60, COLOR_BLACK);

  int wifiX = 8;
  int wifiY = 6;
  for (int i = 0; i < 3; i++) {
    int height = 6 + (i * 4);
    uint16_t color = (bars > i) ? COLOR_GREEN : COLOR_WHITE;
    gfx->fillRect(wifiX + (i * 8), wifiY + (18 - height), 6, height, color);
  }
  gfx->setTextSize(1);
  gfx->setCursor(wifiX, wifiY + 24);
  gfx->print(ssid);
  gfx->setTextSize(2);

  int batteryWidth = 34;
  int batteryHeight = 16;
  int batteryX = screenW - batteryWidth - 16;
  int batteryY = 8;

  gfx->drawRect(batteryX, batteryY, batteryWidth, batteryHeight, COLOR_WHITE);
  gfx->fillRect(batteryX + batteryWidth, batteryY + (batteryHeight / 3), 4,
                batteryHeight / 3, COLOR_WHITE);

  int fillWidth = static_cast<int>((batteryWidth - 4) * (batteryPercent / 100.0f));
  gfx->fillRect(batteryX + 2, batteryY + 2, fillWidth, batteryHeight - 4,
                COLOR_GREEN);
  String batteryStr = String(batteryPercent) + "%";
  int16_t tbx, tby;
  uint16_t tbw, tbh;
  gfx->getTextBounds(batteryStr.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
  int labelX = batteryX + batteryWidth + 8;
  int maxLabelX = screenW - static_cast<int>(tbw) - 2;
  if (labelX > maxLabelX) labelX = maxLabelX;
  int labelY = batteryY + (batteryHeight - static_cast<int>(tbh)) / 2;
  gfx->setCursor(labelX, labelY);
  gfx->print(batteryStr);
}

float readBatteryPercent() {
  auto readMv = [](int pin) -> uint16_t {
    if (pin < 0) return 0;
    return analogReadMilliVolts(pin);
  };

  uint16_t mvPrimary = readMv(BATTERY_ADC_PIN_PRIMARY);
  uint16_t mvAlt = readMv(BATTERY_ADC_PIN_ALT);
  uint16_t millivolts = mvPrimary;
  if (millivolts == 0 && mvAlt > 0) {
    millivolts = mvAlt;
  } else if (mvAlt > millivolts) {
    millivolts = mvAlt;
  }

  float voltage = (millivolts / 1000.0f) * BATTERY_DIVIDER_RATIO;
  // Rolling average over ~20s (sampled every ~2s) for smoother display
  if (batteryAvgVoltage <= 0.01f) {
    batteryAvgVoltage = voltage;
  } else {
    // Simple low-pass filter (alpha ~0.2)
    batteryAvgVoltage = (batteryAvgVoltage * 0.8f) + (voltage * 0.2f);
  }
  float percent = ((voltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) * 100.0f;
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;
  return percent;
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  long rssi = WiFi.RSSI();
  if (rssi > -55) return 3;
  if (rssi > -70) return 2;
  return 1;
}

bool hasInternetConnectivity() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin("http://connectivitycheck.gstatic.com/generate_204");
  http.setTimeout(HTTP_TIMEOUT_MS);
  int status = http.GET();
  http.end();
  return status == HTTP_CODE_NO_CONTENT;
}

void loadSavedNetworks() {
  knownNetworks.clear();
  String saved = prefs.getString("networks", "");
  if (!saved.length()) return;

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, saved) != DeserializationError::Ok) {
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    knownNetworks.emplace_back(obj["s"].as<String>(), obj["p"].as<String>());
  }
}

void saveNetworks() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (const auto &item : knownNetworks) {
    JsonObject obj = arr.createNestedObject();
    obj["s"] = item.first;
    obj["p"] = item.second;
  }

  String payload;
  serializeJson(doc, payload);
  prefs.putString("networks", payload);
}

void ensureNetwork(const String &ssid, const String &password) {
  for (const auto &net : knownNetworks) {
    if (net.first == ssid) {
      // Update existing password if changed
      const_cast<String &>(net.second) = password;
      saveNetworks();
      return;
    }
  }
  knownNetworks.emplace_back(ssid, password);
  saveNetworks();
}

int findNetworkIndex(const String &ssid) {
  for (size_t i = 0; i < knownNetworks.size(); i++) {
    if (knownNetworks[i].first == ssid) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool readTouch(int16_t &x, int16_t &y) {
  uint8_t fingers = 0, evt = 0;
  return readTouch(x, y, fingers, evt);
}

bool readTouch(int16_t &x, int16_t &y, uint8_t &fingers, uint8_t &touchEvent) {
  // Poll the AXS15231B over I2C, triggered by INT falling edge or a periodic poll.
  bool shouldPoll = g_touchInterrupt || millis() >= nextTouchPollMs;
  if (!shouldPoll) return false;

  g_touchInterrupt = false;
  nextTouchPollMs = millis() + 10;  // faster poll for responsiveness

  // Write touch read command
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(TOUCH_CMD, sizeof(TOUCH_CMD));
  // Keep bus active for a repeated start into the read phase.
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  // Read 8-byte touch status payload
  constexpr uint8_t READ_LEN = 8;
  uint8_t buf[READ_LEN] = {0};
  int readCount = Wire.requestFrom(static_cast<int>(TOUCH_ADDR), static_cast<int>(READ_LEN), static_cast<int>(true));
  if (readCount != READ_LEN) {
    if (SERIAL_VERBOSE && Serial && (millis() % 1000 < 50)) {
      Serial.printf("Touch read short: %d/8\n", readCount);
    }
    return false;
  }
  for (int i = 0; i < READ_LEN; i++) {
    buf[i] = Wire.read();
  }

  // Decode using LilyGO macros (event in bits 7-6 of buf[2]).
  // In their reference, X comes from buf[4]/buf[5], Y from buf[2]/buf[3], and Y is inverted vs. panel height.
  fingers = buf[1];  // use full byte; some frames set high nibble
  touchEvent = buf[2] >> 6;  // 0: down, 1: up, 2: contact/move (chip may report other codes; we don't filter hard)
  uint16_t rawX = static_cast<uint16_t>((buf[4] & 0x0F) << 8) | buf[5];
  uint16_t rawY = static_cast<uint16_t>((buf[2] & 0x0F) << 8) | buf[3];

  // Require at least one finger; don't aggressively drop other frames (some chips send varied patterns).
  if (fingers == 0) {
    return false;
  }

  // Panel is 180 (W) x 640 (H); incoming coords are 0..179 (X) and 0..639 (Y), Y inverted.
  x = static_cast<int16_t>(rawX);
  y = static_cast<int16_t>(640 - rawY);
  // Clamp to screen bounds.
  if (x < 0) x = 0;
  if (x >= screenW) x = screenW - 1;
  if (y < 0) y = 0;
  if (y >= screenH) y = screenH - 1;

  if (SERIAL_VERBOSE) {
    Serial.printf("Touch fingers=%u event=%u rawX=%u rawY=%u -> x=%d y=%d\n", fingers, touchEvent, rawX, rawY, x, y);
  }
  return true;
}

bool isWifiIconTouched(int16_t x, int16_t y) {
  // Larger target for easier taps/presses.
  int iconWidth = 64;
  int iconHeight = 48;
  return (x >= 0 && x <= iconWidth && y >= 0 && y <= iconHeight);
}

void openWiFiSettings() {
  wifiSettingsActive = true;
  gfx->fillScreen(COLOR_BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(10, 10);
  gfx->print("WiFi Settings");

  gfx->setCursor(10, 40);
  gfx->print("Scanning...");
  gfx->fillRect(0, 30, screenW, screenH - 30, COLOR_BLACK);

  std::vector<String> ssids = scanUniqueNetworks();

  if (ssids.empty()) {
    gfx->setCursor(10, 50);
    gfx->print("No networks found");
    delay(1500);
    wifiSettingsActive = false;
    drawScaffold();
    return;
  }

  int startY = 60;
  int rowHeight = 44;
  int visibleRows = (screenH - startY - 80) / rowHeight;
  if (visibleRows < 1) visibleRows = 1;
  gfx->setTextSize(3);
  drawNetworkList(ssids, 0, startY, rowHeight, visibleRows);

  gfx->setTextSize(2);
  gfx->setCursor(10, screenH - 30);
  gfx->print("Tap a network (use arrows)");

  clearTouchQueue();
  int choice = selectNetworkFromList(ssids, startY, rowHeight);
  if (choice < 0 || choice >= static_cast<int>(ssids.size())) {
    wifiSettingsActive = false;
    drawScaffold();
    return;
  }

  String chosenSsid = ssids[choice];
  clearTouchQueue();
  String password = promptForPassword(chosenSsid);
  ensureNetwork(chosenSsid, password);
  int newIndex = findNetworkIndex(chosenSsid);
  if (newIndex >= 0) {
    currentNetworkIndex = static_cast<size_t>(newIndex);
  }
  awaitingNewNetworkToken = true;
  newNetworkStartMs = millis();
  accessToken = "";
  tokenExpiresAt = 0;
  connectWiFi();
  drawScaffold();
  wifiSettingsActive = false;
}

int selectNetworkFromList(const std::vector<String> &ssids, int startY, int rowHeight) {
  int total = static_cast<int>(ssids.size());
  if (total == 0) return -1;

  int visibleRows = (screenH - startY - 80) / rowHeight;
  if (visibleRows < 1) visibleRows = 1;
  int startIndex = 0;
  unsigned long timeout = millis() + 20000;

  drawNetworkList(ssids, startIndex, startY, rowHeight, visibleRows);

  while (millis() < timeout) {
    TouchEvent evt;
    if (touchQueue && xQueueReceive(touchQueue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Up arrow area
      if (evt.y >= 20 && evt.y < startY - 10) {
        if (startIndex > 0) {
          startIndex--;
          drawNetworkList(ssids, startIndex, startY, rowHeight, visibleRows);
        }
        continue;
      }
      // Down arrow area
      if (evt.y > screenH - 60) {
        if (startIndex + visibleRows < total) {
          startIndex++;
          drawNetworkList(ssids, startIndex, startY, rowHeight, visibleRows);
        }
        continue;
      }
      // Row hit
      if (evt.y >= startY && evt.y < startY + visibleRows * rowHeight) {
        int index = startIndex + (evt.y - startY) / rowHeight;
        if (index >= 0 && index < total) {
          return index;
        }
      }
    }
  }
  return -1;
}

String promptForPassword(const String &ssid) {
  static const char charset[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*-_+=.? ";
  const int charsetLen = sizeof(charset) - 1;
  int charIndex = 0;
  String password;

  clearTouchQueue();
  unsigned long entryMs = millis();

  auto redraw = [&](const String &pwd, char current) {
    gfx->fillRect(0, 30, screenW, screenH - 30, COLOR_BLACK);
    gfx->setTextSize(2);
    gfx->setCursor(10, 40);
    gfx->printf("Password for:\n%s", ssid.c_str());
    gfx->setCursor(10, 90);
    gfx->print("Current:");
    gfx->setCursor(10, 120);
    gfx->setTextSize(3);
    String display = pwd;
    if (display.length() > 12) {
      display = String("...") + display.substring(display.length() - 12);
    }
    gfx->print(display);

    // Spinner
    int spinnerY = 220;
    gfx->setTextSize(6);
    gfx->setCursor((screenW / 2) - 20, spinnerY - 90);
    gfx->print("^");
    gfx->setCursor((screenW / 2) - 20, spinnerY);
    gfx->printf("%c", current);
    gfx->setCursor((screenW / 2) - 20, spinnerY + 90);
    gfx->print("v");

    // Buttons layout:
    // Add (full width) near top of control area
    // Bottom: Done, then Back, gap, then Cancel (full width bars)
    gfx->setTextSize(3);
    int addH = 70;
    int addTopY = screenH - 260;
    if (addTopY < 140) addTopY = 140;  // keep below header text
    gfx->fillRect(0, addTopY, screenW, addH, COLOR_BLACK);
    gfx->setCursor((screenW / 2) - 30, addTopY + 20);
    gfx->print("Add");

    int rowH = 70;
    int doneY = screenH - 3 * rowH;   // Done
    int backY = screenH - 2 * rowH;   // Back
    int cancelY = screenH - rowH;     // Cancel (gap appears automatically above it)
    gfx->fillRect(0, doneY, screenW, rowH, COLOR_BLACK);
    gfx->fillRect(0, backY, screenW, rowH, COLOR_BLACK);
    gfx->fillRect(0, cancelY, screenW, rowH, COLOR_BLACK);

    gfx->setCursor(10, doneY + 20);
    gfx->print("Done");
    gfx->setCursor(10, backY + 20);
    gfx->print("Back");
    gfx->setCursor(10, cancelY + 20);
    gfx->print("Cancel");
  };

  redraw(password, charset[charIndex]);

  // Wait until user completes or cancels; no timeout.
  while (true) {
    TouchEvent evt;
    if (touchQueue && xQueueReceive(touchQueue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Ignore any early events within 300ms of entry to avoid stale taps.
      if (millis() - entryMs < 300) continue;
      // Only act on down/up events.
      if (!(evt.event == 0 || evt.event == 1)) continue;

      // Spinner up
      if (evt.y >= 80 && evt.y < 220) {
        charIndex = (charIndex - 1 + charsetLen) % charsetLen;
        redraw(password, charset[charIndex]);
        continue;
      }
      // Spinner down
      if (evt.y >= 240 && evt.y < 380) {
        charIndex = (charIndex + 1) % charsetLen;
        redraw(password, charset[charIndex]);
        continue;
      }
      // Buttons hit zones
      int addTopY = screenH - 260;
      int addH = 70;
      if (addTopY < 140) addTopY = 140;
      int rowH = 70;
      int doneY = screenH - 3 * rowH;
      int backY = screenH - 2 * rowH;
      int cancelY = screenH - rowH;

      if (evt.y >= addTopY && evt.y < addTopY + addH) {
        password += charset[charIndex];
        redraw(password, charset[charIndex]);
      } else if (evt.y >= doneY && evt.y < doneY + rowH) {
        return password;  // Done
      } else if (evt.y >= backY && evt.y < backY + rowH) {
        if (password.length() > 0) password.remove(password.length() - 1);
        redraw(password, charset[charIndex]);
      } else if (evt.y >= cancelY && evt.y < cancelY + rowH) {
        return "";
      }
    }
  }
  return password;
}

void showStatus(const String &msg) {
  gfx->fillRect(0, screenH - 18, screenW, 18, COLOR_BLACK);
  gfx->setCursor(4, screenH - 16);
  gfx->setTextColor(COLOR_YELLOW, COLOR_BLACK);
  gfx->print(msg);
  gfx->setTextColor(COLOR_WHITE, COLOR_BLACK);
  if (SERIAL_VERBOSE) {
    Serial.println(msg);
  }
}

std::vector<String> scanUniqueNetworks() {
  std::vector<String> ssids;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.setSleep(false);
  delay(200);

  int count = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (count <= 0) {
    delay(700);
    count = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  }

  for (int i = 0; i < count; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool exists = false;
    for (const auto &t : ssids) {
      if (t == s) {
        exists = true;
        break;
      }
    }
    if (!exists) ssids.push_back(s);
  }
  return ssids;
}

void drawNetworkList(const std::vector<String> &ssids, int startIndex, int startY, int rowHeight, int visibleRows) {
  int listHeight = visibleRows * rowHeight;
  if (listHeight < 0) listHeight = 0;
  int areaHeight = listHeight + 60;
  gfx->fillRect(0, startY - 30, screenW, areaHeight + 40, COLOR_BLACK);

  gfx->setTextSize(2);
  // Up/down arrows
  gfx->setCursor((screenW / 2) - 6, startY - 24);
  gfx->print("^");
  gfx->setCursor((screenW / 2) - 6, screenH - 40);
  gfx->print("v");

  gfx->setTextSize(3);
  for (int i = 0; i < visibleRows; i++) {
    int idx = startIndex + i;
    if (idx >= static_cast<int>(ssids.size())) break;
    gfx->setCursor(8, startY + (i * rowHeight));
    gfx->printf("%d: %s", idx + 1, ssids[idx].c_str());
  }

  gfx->setTextSize(1);
  gfx->setCursor(8, screenH - 20);
  gfx->printf("Showing %d-%d of %d", startIndex + 1,
              min(startIndex + visibleRows, static_cast<int>(ssids.size())),
              static_cast<int>(ssids.size()));
}

void scanI2CBus() {
  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf(" - Found 0x%02X\n", addr);
    }
  }
}

void printBuf(const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    Serial.printf(" %02X", buf[i]);
  }
  Serial.println();
}

void touchTask(void *param) {
  const TickType_t idleDelay = pdMS_TO_TICKS(5);
  TouchEvent evt{};
  for (;;) {
    int16_t x, y;
    uint8_t fingers = 0, ev = 0;
    bool got = readTouch(x, y, fingers, ev);
    if (got) {
      evt.x = x;
      evt.y = y;
      evt.fingers = fingers;
      evt.event = ev;
      if (touchQueue) {
        if (xQueueSend(touchQueue, &evt, 0) != pdPASS) {
          xQueueReset(touchQueue);
          xQueueSend(touchQueue, &evt, 0);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    } else {
      vTaskDelay(idleDelay);
    }
  }
}

void clearTouchQueue() {
  if (!touchQueue) return;
  TouchEvent evt;
  while (xQueueReceive(touchQueue, &evt, 0) == pdTRUE) {
  }
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'W' || c == 'w') {
      Serial.println("WiFi networks (id: ssid / password):");
      for (size_t i = 0; i < knownNetworks.size(); i++) {
        Serial.printf("%u: %s / %s\n", static_cast<unsigned>(i), knownNetworks[i].first.c_str(),
                      knownNetworks[i].second.c_str());
      }
      if (knownNetworks.empty()) {
        Serial.println("(none)");
      }
    } else if (c == 'D' || c == 'd') {
      // Parse the rest of the line as an index
      String num = Serial.readStringUntil('\n');
      num.trim();
      int idx = num.toInt();
      if (idx >= 0 && idx < static_cast<int>(knownNetworks.size())) {
        Serial.printf("Deleting WiFi id %d (%s)\n", idx, knownNetworks[idx].first.c_str());
        knownNetworks.erase(knownNetworks.begin() + idx);
        // Adjust current/last working indices if needed
        if (currentNetworkIndex >= knownNetworks.size()) {
          currentNetworkIndex = 0;
        }
        if (hasLastWorkingNetwork && lastWorkingNetworkIndex >= knownNetworks.size()) {
          hasLastWorkingNetwork = false;
          lastWorkingNetworkIndex = 0;
        }
        saveNetworks();
      } else {
        Serial.printf("Invalid delete id: %s\n", num.c_str());
      }
    }
  }
}
