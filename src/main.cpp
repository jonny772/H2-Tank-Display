#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
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

// Pressure scaling
constexpr float MAX_BAR_VALUE = 1.5f;  // 100%

TFT_eSPI tft;
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
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  drawScaffold();
  showStatus("WiFi...");

  connectWiFi();

  secureClient.setInsecure();
  showStatus("Auth...");
  refreshAccessToken();
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

  String body = String("grant_type=client_credentials&client_id=") + CLIENT_ID + "&client_secret=" + CLIENT_SECRET;
  int status = http.POST(body);
  if (status != HTTP_CODE_OK) {
    showStatus("Token err");
    Serial.printf("Token request failed: %d\n", status);
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
  const char *thingsUrl = "https://api2.arduino.cc/iot/v2/things?show_properties=true";

  http.begin(secureClient, thingsUrl);
  http.addHeader("Authorization", String("Bearer ") + accessToken);

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    showStatus("Data err");
    Serial.printf("Thing request failed: %d\n", status);
    http.end();
    return -1.0f;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err) {
    showStatus("Data parse");
    Serial.printf("Thing parse error: %s\n", err.c_str());
    http.end();
    return -1.0f;
  }

  float value = -1.0f;
  for (JsonObject thing : doc.as<JsonArray>()) {
    const char *name = thing["name"] | "";
    if (strcmp(name, THING_NAME) != 0) {
      continue;
    }
    for (JsonObject prop : thing["properties"].as<JsonArray>()) {
      const char *propName = prop["name"] | "";
      if (strcmp(propName, PROPERTY_NAME) == 0) {
        value = prop["last_value"].as<float>();
        break;
      }
    }
  }

  http.end();

  if (value < 0.0f) {
    showStatus("No data");
    return -1.0f;
  }

  return value;
}

void drawScaffold() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("H2 Tank", tft.width() / 2, 10);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Pressure (bar)", 4, 30);

  int barTop = 60;
  int barHeight = tft.height() - barTop - 20;
  int barWidth = 40;
  int barX = tft.width() - barWidth - 20;

  tft.drawRect(barX - 2, barTop - 2, barWidth + 4, barHeight + 4, TFT_WHITE);
  tft.drawString("0", barX + barWidth + 8, barTop + barHeight - 8);
  tft.drawString("1.5", barX + barWidth + 4, barTop - 8);
}

void drawBar(float valueBar) {
  int barTop = 60;
  int barHeight = tft.height() - barTop - 20;
  int barWidth = 40;
  int barX = tft.width() - barWidth - 20;
  int barY = barTop;

  float clamped = valueBar;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > MAX_BAR_VALUE) clamped = MAX_BAR_VALUE;

  float percent = (clamped / MAX_BAR_VALUE);
  int filled = static_cast<int>(barHeight * percent);

  // clear bar area
  tft.fillRect(barX, barY, barWidth, barHeight, TFT_BLACK);
  // draw filled portion from bottom
  tft.fillRect(barX, barY + (barHeight - filled), barWidth, filled, TFT_GREEN);
  tft.drawRect(barX, barY, barWidth, barHeight, TFT_WHITE);

  tft.fillRect(0, 60, tft.width() - barWidth - 30, tft.height() - 80, TFT_BLACK);
  tft.setCursor(4, 60);
  tft.printf("Reading: %.2f bar\n", valueBar);
  tft.printf("Level:  %.0f%%", percent * 100.0f);
}

void showStatus(const String &msg) {
  tft.fillRect(0, tft.height() - 20, tft.width(), 20, TFT_BLACK);
  tft.setCursor(4, tft.height() - 18);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print(msg);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  Serial.println(msg);
}
