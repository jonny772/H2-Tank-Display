#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include "display/Arduino_AXS15231B.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <vector>

// Wi-Fi credentials
constexpr char WIFI_SSID[] = "LUinc-Members";
constexpr char WIFI_PASSWORD[] = "eFGEpC-uH2";

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

// Battery monitor
constexpr int BATTERY_ADC_PIN = 4;
constexpr float BATTERY_MIN_V = 3.3f;
constexpr float BATTERY_MAX_V = 4.2f;

// Color aliases
constexpr uint16_t COLOR_BLACK = RGB565_BLACK;
constexpr uint16_t COLOR_WHITE = RGB565_WHITE;
constexpr uint16_t COLOR_GREEN = RGB565_GREEN;
constexpr uint16_t COLOR_YELLOW = RGB565_YELLOW;

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

void connectWiFi();
bool refreshAccessToken();
float fetchPressure();
void drawScaffold();
void drawBar(float valueBar);
void showStatus(const String &msg);
void drawStatusIcons();
float readBatteryPercent();
int wifiBars();
bool hasInternetConnectivity();
void loadSavedNetworks();
void saveNetworks();
void ensureNetwork(const String &ssid, const String &password);
bool readTouch(int16_t &x, int16_t &y);
bool isWifiIconTouched(int16_t x, int16_t y);
void openWiFiSettings();
int selectNetworkFromList(int networkCount, int startY, int rowHeight);
String promptForPassword(const String &ssid);

void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  ledcAttachPin(TFT_BL, BACKLIGHT_PWM_CHANNEL);
  ledcSetup(BACKLIGHT_PWM_CHANNEL, 2000, 8);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);  // keep off until the panel is ready

  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

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
  showStatus("Auth...");
  refreshAccessToken();

  // Fade backlight up
  for (int i = 0; i <= 255; i += 8) {
    ledcWrite(BACKLIGHT_PWM_CHANNEL, i);
    delay(5);
  }
}

void loop() {
  int16_t tx, ty;
  if (readTouch(tx, ty)) {
    lastTouchX = tx;
    lastTouchY = ty;
    if (isWifiIconTouched(tx, ty)) {
      openWiFiSettings();
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("Reconnect WiFi");
    connectWiFi();
  }

  if (millis() - lastConnectivityCheck > connectivityIntervalMs) {
    lastConnectivityCheck = millis();
    if (!hasInternetConnectivity() && !knownNetworks.empty()) {
      showStatus("Find internet");
      currentNetworkIndex = (currentNetworkIndex + 1) % knownNetworks.size();
      connectWiFi();
    }
  }

  if (millis() > tokenExpiresAt) {
    showStatus("Refresh token");
    if (!refreshAccessToken()) {
      delay(5000);
      return;
    }
  }

  float pressure = fetchPressure();
  if (pressure >= 0.0f) {
    lastPressureReading = pressure;
    drawBar(pressure);
  }

  drawStatusIcons();
  delay(5000);
}

void connectWiFi() {
  if (knownNetworks.empty()) {
    ensureNetwork(WIFI_SSID, WIFI_PASSWORD);
  }

  for (size_t i = 0; i < knownNetworks.size(); i++) {
    size_t attemptIndex = (currentNetworkIndex + i) % knownNetworks.size();
    const auto &cred = knownNetworks[attemptIndex];
    if (cred.first.isEmpty()) continue;

    showStatus(String("Join ") + cred.first);
    WiFi.begin(cred.first.c_str(), cred.second.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 60) {
      delay(250);
      attempts++;
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

  DynamicJsonDocument doc(2048);
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

  DynamicJsonDocument doc(768);
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
  int barTop = 130;
  int barHeight = 120;
  int barLeft = 10;
  int barWidth = screenW - (barLeft * 2);

  float clamped = valueBar;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > MAX_BAR_VALUE) clamped = MAX_BAR_VALUE;

  float percent = (clamped / MAX_BAR_VALUE) * 100.0f;
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  int filled = static_cast<int>(barWidth * (percent / 100.0f));

  gfx->fillRect(barLeft, barTop, barWidth, barHeight, COLOR_BLACK);
  gfx->drawRoundRect(barLeft, barTop, barWidth, barHeight, 8, COLOR_WHITE);
  if (filled > 0) {
    gfx->fillRoundRect(barLeft, barTop, filled, barHeight, 8, COLOR_GREEN);
  }

  gfx->fillRect(0, barTop + barHeight + 10, screenW, 60, COLOR_BLACK);
  gfx->setCursor(barLeft, barTop + barHeight + 20);
  gfx->setTextSize(3);
  gfx->printf("%.0f%%", percent);
  gfx->setTextSize(2);
}

void drawStatusIcons() {
  gfx->fillRect(0, 0, screenW, 60, COLOR_BLACK);

  int wifiX = 8;
  int wifiY = 6;
  int bars = wifiBars();
  for (int i = 0; i < 3; i++) {
    int height = 6 + (i * 4);
    uint16_t color = (bars > i) ? COLOR_GREEN : COLOR_WHITE;
    gfx->fillRect(wifiX + (i * 8), wifiY + (18 - height), 6, height, color);
  }
  gfx->setCursor(wifiX, wifiY + 24);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->print(WiFi.SSID());
  } else {
    gfx->print("No WiFi");
  }

  int batteryWidth = 34;
  int batteryHeight = 16;
  int batteryX = screenW - batteryWidth - 16;
  int batteryY = 8;

  gfx->drawRect(batteryX, batteryY, batteryWidth, batteryHeight, COLOR_WHITE);
  gfx->fillRect(batteryX + batteryWidth, batteryY + (batteryHeight / 3), 4,
                batteryHeight / 3, COLOR_WHITE);

  float batteryPercent = readBatteryPercent();
  int fillWidth = static_cast<int>((batteryWidth - 4) * (batteryPercent / 100.0f));
  gfx->fillRect(batteryX + 2, batteryY + 2, fillWidth, batteryHeight - 4,
                COLOR_GREEN);
  gfx->setCursor(batteryX - 6, batteryY + batteryHeight + 10);
  gfx->printf("%.0f%%", batteryPercent);
}

float readBatteryPercent() {
  uint16_t millivolts = analogReadMilliVolts(BATTERY_ADC_PIN);
  float voltage = millivolts / 1000.0f;
  float percent = ((voltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) *
                  100.0f;
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
      return;
    }
  }
  knownNetworks.emplace_back(ssid, password);
  saveNetworks();
}

bool readTouch(int16_t &x, int16_t &y) {
#ifdef TOUCH_INT
  // If a touch driver is wired, poll it here. Placeholder for integration.
  return false;
#else
  if (Serial.available()) {
    char command = Serial.peek();
    if (command == 'w' || command == 'W') {
      Serial.read();
      x = 0;
      y = 0;
      return true;
    }
  }
  (void)x;
  (void)y;
  return false;
#endif
}

bool isWifiIconTouched(int16_t x, int16_t y) {
  int iconWidth = 28;
  int iconHeight = 26;
  return (x >= 0 && x <= iconWidth && y >= 0 && y <= iconHeight);
}

void openWiFiSettings() {
  gfx->fillScreen(COLOR_BLACK);
  gfx->setCursor(10, 10);
  gfx->print("WiFi Settings");

  gfx->setCursor(10, 34);
  gfx->print("Scanning...");
  int networkCount = WiFi.scanNetworks();
  gfx->fillRect(0, 30, screenW, screenH - 30, COLOR_BLACK);

  if (networkCount <= 0) {
    gfx->setCursor(10, 50);
    gfx->print("No networks found");
    delay(1500);
    drawScaffold();
    return;
  }

  int startY = 40;
  int rowHeight = 26;
  for (int i = 0; i < networkCount && (startY + (i * rowHeight)) < screenH - 30; i++) {
    gfx->setCursor(10, startY + (i * rowHeight));
    gfx->printf("%d: %s", i + 1, WiFi.SSID(i).c_str());
  }

  gfx->setCursor(10, screenH - 30);
  gfx->print("Tap a network");

  int choice = selectNetworkFromList(networkCount, startY, rowHeight);
  if (choice < 0 || choice >= networkCount) {
    drawScaffold();
    return;
  }

  String chosenSsid = WiFi.SSID(choice);
  String password = promptForPassword(chosenSsid);
  ensureNetwork(chosenSsid, password);
  currentNetworkIndex = 0;
  connectWiFi();
  drawScaffold();
}

int selectNetworkFromList(int networkCount, int startY, int rowHeight) {
  unsigned long timeout = millis() + 15000;
  while (millis() < timeout) {
    int16_t x, y;
    if (readTouch(x, y)) {
      int index = (y - startY) / rowHeight;
      if (index >= 0 && index < networkCount) {
        return index;
      }
    }
    delay(50);
  }
  return -1;
}

String promptForPassword(const String &ssid) {
  gfx->fillRect(0, 30, screenW, screenH - 30, COLOR_BLACK);
  gfx->setCursor(10, 40);
  gfx->printf("Enter password for %s\n", ssid.c_str());
  gfx->setCursor(10, 68);
  gfx->print("Send via Serial");

  String password;
  unsigned long timeout = millis() + 30000;
  while (millis() < timeout) {
    if (Serial.available()) {
      password = Serial.readStringUntil('\n');
      password.trim();
      break;
    }
    delay(50);
  }

  return password;
}

void showStatus(const String &msg) {
  gfx->fillRect(0, screenH - 18, screenW, 18, COLOR_BLACK);
  gfx->setCursor(4, screenH - 16);
  gfx->setTextColor(COLOR_YELLOW, COLOR_BLACK);
  gfx->print(msg);
  gfx->setTextColor(COLOR_WHITE, COLOR_BLACK);
  Serial.println(msg);
}
