#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include "display/Arduino_AXS15231B.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>

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
String accessToken;
unsigned long tokenExpiresAt = 0;  // millis when token expires

void connectWiFi();
bool refreshAccessToken();
float fetchPressure();
void drawScaffold();
void drawBar(float valueBar);
void showStatus(const String &msg);

void setup() {
  Serial.begin(115200);

  // Backlight
  pinMode(TFT_BL, OUTPUT);
  ledcAttachPin(TFT_BL, BACKLIGHT_PWM_CHANNEL);
  ledcSetup(BACKLIGHT_PWM_CHANNEL, 2000, 8);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);  // keep off until the panel is ready

  gfx->begin();
  screenW = gfx->width();
  screenH = gfx->height();
  gfx->fillScreen(COLOR_BLACK);
  gfx->setTextColor(COLOR_WHITE, COLOR_BLACK);
  gfx->setTextSize(2);

  drawScaffold();
  showStatus("WiFi...");

  connectWiFi();

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
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("Reconnect WiFi");
    connectWiFi();
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
    drawBar(pressure);
  }

  delay(5000);
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(250);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    showStatus("WiFi OK");
  } else {
    showStatus("WiFi fail");
  }
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

  gfx->setCursor((screenW / 2) - 40, 6);
  gfx->print("H2 Tank");
  gfx->setCursor(6, 26);
  gfx->print("Pressure (bar)");

  int barTop = 60;
  int barHeight = screenH - barTop - 30;
  int barWidth = 30;
  int barX = screenW - barWidth - 18;

  gfx->drawRect(barX - 2, barTop - 2, barWidth + 4, barHeight + 4, COLOR_WHITE);
  gfx->setCursor(barX + barWidth + 8, barTop + barHeight - 10);
  gfx->print("0");
  gfx->setCursor(barX + barWidth + 8, barTop - 10);
  gfx->print("1.5");
}

void drawBar(float valueBar) {
  int barTop = 50;
  int barHeight = screenH - barTop - 24;
  int barWidth = 30;
  int barX = screenW - barWidth - 18;
  int barY = barTop;

  float clamped = valueBar;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > MAX_BAR_VALUE) clamped = MAX_BAR_VALUE;

  float percent = (clamped / MAX_BAR_VALUE);
  int filled = static_cast<int>(barHeight * percent);

  // clear bar area
  gfx->fillRect(barX, barY, barWidth, barHeight, COLOR_BLACK);
  // draw filled portion from bottom
  gfx->fillRect(barX, barY + (barHeight - filled), barWidth, filled, COLOR_GREEN);
  gfx->drawRect(barX, barY, barWidth, barHeight, COLOR_WHITE);

  gfx->fillRect(0, barTop, screenW - barWidth - 32, screenH - barTop - 20, COLOR_BLACK);
  gfx->setCursor(6, barTop + 4);
  gfx->printf("Reading: %.2f bar\n", valueBar);
  gfx->printf("Level:   %.0f%%", percent * 100.0f);
}

void showStatus(const String &msg) {
  gfx->fillRect(0, screenH - 18, screenW, 18, COLOR_BLACK);
  gfx->setCursor(4, screenH - 16);
  gfx->setTextColor(COLOR_YELLOW, COLOR_BLACK);
  gfx->print(msg);
  gfx->setTextColor(COLOR_WHITE, COLOR_BLACK);
  Serial.println(msg);
}
