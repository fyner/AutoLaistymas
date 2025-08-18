struct Config;
enum SystemStateEnum : uint8_t;

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "RTClib.h"
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

#define WATER_LEVEL_PIN 13
#define RELAY_PIN 12

#define MAX_DEBOUNCE_SAMPLES 10
#define MAX_WATERING_SLOTS 8

bool parseIsoDateTime(const String &iso, DateTime &out);

enum SystemStateEnum : uint8_t {
  STATE_IDLE,
  STATE_WINDOW_OPEN,
  STATE_WATERING,
  STATE_ERROR_PAUSED,
  STATE_UNKNOWN
};

static inline SystemStateEnum systemStateFromString(String stateStr) {
  if (stateStr == "Idle") return STATE_IDLE;
  if (stateStr == "WindowOpen") return STATE_WINDOW_OPEN;
  if (stateStr == "Watering") return STATE_WATERING;
  if (stateStr == "ErrorPaused") return STATE_ERROR_PAUSED;
  return STATE_UNKNOWN;
}

static inline String systemStateToString(SystemStateEnum stateEnum) {
  switch (stateEnum) {
    case STATE_IDLE: return "Idle";
    case STATE_WINDOW_OPEN: return "WindowOpen";
    case STATE_WATERING: return "Watering";
    case STATE_ERROR_PAUSED: return "ErrorPaused";
    default: return "Unknown";
  }
}

bool parseIsoDateTime(const String &iso, DateTime &out) {
  if (iso.length() < 19) return false;
  int Y = iso.substring(0, 4).toInt();
  int M = iso.substring(5, 7).toInt();
  int D = iso.substring(8, 10).toInt();
  int h = iso.substring(11, 13).toInt();
  int m = iso.substring(14, 16).toInt();
  int s = iso.substring(17, 19).toInt();
  if (iso.charAt(4) != '-' || iso.charAt(7) != '-' ||
      (iso.charAt(10) != 'T' && iso.charAt(10) != 't') ||
      iso.charAt(13) != ':' || iso.charAt(16) != ':') {
    return false;
  }
  if (Y < 2000 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) {
    return false;
  }
  out = DateTime(Y, M, D, h, m, s);
  return out.isValid();
}

struct WaterLevelConfig {
  String minState;
  int debounceSamples;
  int debounceIntervalMs;
  String pullMode;
};

struct BME280Config {
  float tempMin, tempMax;
  float humMin, humMax;
  float presMin, presMax;
};

struct WifiConfig {
  String apSsid;
  String apPassword;
  int apChannel;
  bool apHidden;
};

struct RelayConfig {
  String activeLevel;
};

struct Config {
  String time;
  int wateringDurationMin;
  int toleranceWindowMin;
  int sensorReadIntervalMs;
  int pauseResumeCheckIntervalMs;
  int wateringTimesCount;
  String wateringTimes[MAX_WATERING_SLOTS];
  WaterLevelConfig waterLevel;
  BME280Config bme280;
  WifiConfig wifi;
  RelayConfig relay;

};

Config currentConfig;

static inline int getEffectiveDebounceSamples() {
  int s = currentConfig.waterLevel.debounceSamples;
  if (s < 1) return 1;
  if (s > MAX_DEBOUNCE_SAMPLES) return MAX_DEBOUNCE_SAMPLES;
  return s;
}

static inline void configToJson(JsonDocument &doc, const Config &cfg, bool includeSensitive = false) {
  doc["time"] = cfg.time;
  doc["wateringDurationMin"] = cfg.wateringDurationMin;
  doc["toleranceWindowMin"] = cfg.toleranceWindowMin;
  doc["sensorReadIntervalMs"] = cfg.sensorReadIntervalMs;
  doc["pauseResumeCheckIntervalMs"] = cfg.pauseResumeCheckIntervalMs;
  JsonArray arr = doc.createNestedArray("wateringTimes");
  for (int i = 0; i < cfg.wateringTimesCount && i < MAX_WATERING_SLOTS; i++) {
    if (cfg.wateringTimes[i].length() >= 4) arr.add(cfg.wateringTimes[i]);
  }

  JsonObject waterLevelDoc = doc.createNestedObject("waterLevel");
  waterLevelDoc["minState"] = cfg.waterLevel.minState;
  waterLevelDoc["debounceSamples"] = cfg.waterLevel.debounceSamples;
  waterLevelDoc["debounceIntervalMs"] = cfg.waterLevel.debounceIntervalMs;
  waterLevelDoc["pullMode"] = cfg.waterLevel.pullMode;

  JsonObject bme280Doc = doc.createNestedObject("bme280");
  bme280Doc["tempMin"] = cfg.bme280.tempMin;
  bme280Doc["tempMax"] = cfg.bme280.tempMax;
  bme280Doc["humMin"] = cfg.bme280.humMin;
  bme280Doc["humMax"] = cfg.bme280.humMax;
  bme280Doc["presMin"] = cfg.bme280.presMin;
  bme280Doc["presMax"] = cfg.bme280.presMax;

  JsonObject wifiDoc = doc.createNestedObject("wifi");
  wifiDoc["apSsid"] = cfg.wifi.apSsid;
  wifiDoc["apPassword"] = cfg.wifi.apPassword;
  wifiDoc["apChannel"] = cfg.wifi.apChannel;
  wifiDoc["apHidden"] = cfg.wifi.apHidden;

  JsonObject relayDoc = doc.createNestedObject("relay");
  relayDoc["activeLevel"] = cfg.relay.activeLevel;

  if (includeSensitive) {
  
  }
}

static inline bool applyConfigFromJson(const JsonDocument &doc, Config &cfg, bool *outWifiChanged = nullptr) {
  bool wifiChanged = false;
  if (doc.containsKey("time")) cfg.time = doc["time"].as<String>();
  if (doc.containsKey("wateringDurationMin")) cfg.wateringDurationMin = doc["wateringDurationMin"];
  if (doc.containsKey("toleranceWindowMin")) cfg.toleranceWindowMin = doc["toleranceWindowMin"];
  if (doc.containsKey("sensorReadIntervalMs")) cfg.sensorReadIntervalMs = doc["sensorReadIntervalMs"];
  // wateringTimes
  if (doc.containsKey("wateringTimes") && doc["wateringTimes"].is<JsonArrayConst>()) {
    JsonArrayConst a = doc["wateringTimes"].as<JsonArrayConst>();
    int count = 0;
    for (JsonVariantConst v : a) {
      if (!v.is<const char*>()) continue;
      String s = v.as<const char*>();
      if (s.length() == 5 && s.charAt(2) == ':') {
        int h = s.substring(0,2).toInt();
        int m = s.substring(3,5).toInt();
        if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
          if (count < MAX_WATERING_SLOTS) {
            cfg.wateringTimes[count++] = s;
          }
        }
      }
    }
    cfg.wateringTimesCount = count;
  }
  if (doc.containsKey("pauseResumeCheckIntervalMs")) cfg.pauseResumeCheckIntervalMs = doc["pauseResumeCheckIntervalMs"];

  JsonObjectConst waterLevelDoc = doc["waterLevel"].as<JsonObjectConst>();
  if (!waterLevelDoc.isNull()) {
    if (waterLevelDoc.containsKey("minState")) cfg.waterLevel.minState = waterLevelDoc["minState"].as<String>();
    if (waterLevelDoc.containsKey("debounceSamples")) cfg.waterLevel.debounceSamples = waterLevelDoc["debounceSamples"];
    if (waterLevelDoc.containsKey("debounceIntervalMs")) cfg.waterLevel.debounceIntervalMs = waterLevelDoc["debounceIntervalMs"];
    if (waterLevelDoc.containsKey("pullMode")) cfg.waterLevel.pullMode = waterLevelDoc["pullMode"].as<String>();
  }

  JsonObjectConst bme280Doc = doc["bme280"].as<JsonObjectConst>();
  if (!bme280Doc.isNull()) {
    if (bme280Doc.containsKey("tempMin")) cfg.bme280.tempMin = bme280Doc["tempMin"];
    if (bme280Doc.containsKey("tempMax")) cfg.bme280.tempMax = bme280Doc["tempMax"];
    if (bme280Doc.containsKey("humMin")) cfg.bme280.humMin = bme280Doc["humMin"];
    if (bme280Doc.containsKey("humMax")) cfg.bme280.humMax = bme280Doc["humMax"];
    if (bme280Doc.containsKey("presMin")) cfg.bme280.presMin = bme280Doc["presMin"];
    if (bme280Doc.containsKey("presMax")) cfg.bme280.presMax = bme280Doc["presMax"];
  }

  JsonObjectConst wifiDoc = doc["wifi"].as<JsonObjectConst>();
  if (!wifiDoc.isNull()) {
    if (wifiDoc.containsKey("apSsid") && cfg.wifi.apSsid != wifiDoc["apSsid"].as<String>()) {
      cfg.wifi.apSsid = wifiDoc["apSsid"].as<String>();
      wifiChanged = true;
    }
    if (wifiDoc.containsKey("apPassword") && cfg.wifi.apPassword != wifiDoc["apPassword"].as<String>()) {
      cfg.wifi.apPassword = wifiDoc["apPassword"].as<String>();
      wifiChanged = true;
    }
    if (wifiDoc.containsKey("apChannel")) { int v = wifiDoc["apChannel"]; if (cfg.wifi.apChannel != v) { cfg.wifi.apChannel = v; wifiChanged = true; } }
    if (wifiDoc.containsKey("apHidden")) { bool v = wifiDoc["apHidden"]; if (cfg.wifi.apHidden != v) { cfg.wifi.apHidden = v; wifiChanged = true; } }
  }

  JsonObjectConst relayDoc = doc["relay"].as<JsonObjectConst>();
  if (!relayDoc.isNull()) {
    if (relayDoc.containsKey("activeLevel")) cfg.relay.activeLevel = relayDoc["activeLevel"].as<String>();
  }



  if (outWifiChanged) *outWifiChanged = wifiChanged;
  return true;
}

float currentTemperature = -999.0;
float currentHumidity = -999.0;
float currentPressure = -999.0;
String currentWaterLevelState = "UNKNOWN";
SystemStateEnum currentState = STATE_IDLE;
String systemState = systemStateToString(currentState);
unsigned int remainingWateringTimeSec = 0;
DateTime currentDateTime;
DateTime windowEndsAt;
int lastWateringYMDForSlot[MAX_WATERING_SLOTS];
int activeSlotIndex = -1;
bool isManualWatering = false;

bool relayActiveLow = true;
bool relayIsOn = false;
inline void turnRelayOn() {
  digitalWrite(RELAY_PIN, relayActiveLow ? LOW : HIGH);
}
inline void turnRelayOff() {
  digitalWrite(RELAY_PIN, relayActiveLow ? HIGH : LOW);
}

static inline void setState(SystemStateEnum newState) {
  if (newState == currentState) return;
  if (newState == STATE_WATERING) {
    if (!relayIsOn) { turnRelayOn(); relayIsOn = true; }
  } else {
    if (relayIsOn) { turnRelayOff(); relayIsOn = false; }
  }
  currentState = newState;
  systemState = systemStateToString(currentState);
}

void loadDefaultConfig() {
  currentConfig.time = "2025-05-08T08:00:00";
  currentConfig.wateringDurationMin = 60;
  currentConfig.toleranceWindowMin = 120;
  currentConfig.sensorReadIntervalMs = 1000;
  currentConfig.pauseResumeCheckIntervalMs = 1000;
  currentConfig.wateringTimesCount = 2;
  currentConfig.wateringTimes[0] = "06:00";
  currentConfig.wateringTimes[1] = "20:00";

  currentConfig.waterLevel.minState = "HIGH";
  currentConfig.waterLevel.debounceSamples = 5;
  currentConfig.waterLevel.debounceIntervalMs = 50;
  currentConfig.waterLevel.pullMode = "PULLUP";

  currentConfig.bme280.tempMin = 5.0;
  currentConfig.bme280.tempMax = 35.0;
  currentConfig.bme280.humMin = 30.0;
  currentConfig.bme280.humMax = 80.0;
  currentConfig.bme280.presMin = 950.0;
  currentConfig.bme280.presMax = 1050.0;

  currentConfig.wifi.apSsid = "AutoLaistymas";
  currentConfig.wifi.apPassword = "esp32automatinis";
  currentConfig.wifi.apChannel = 1;
  currentConfig.wifi.apHidden = false;
  currentConfig.relay.activeLevel = "LOW";

  
  Serial.println("Loaded default configuration.");
}

static inline void validateAndClampConfig(Config &cfg) {
  if (cfg.wateringDurationMin < 1) cfg.wateringDurationMin = 1;
  if (cfg.toleranceWindowMin < 1) cfg.toleranceWindowMin = 1;
  if (cfg.sensorReadIntervalMs < 100) cfg.sensorReadIntervalMs = 100;
  if (cfg.pauseResumeCheckIntervalMs < 200) cfg.pauseResumeCheckIntervalMs = 200;
  if (cfg.wateringTimesCount < 0) cfg.wateringTimesCount = 0;
  if (cfg.wateringTimesCount > MAX_WATERING_SLOTS) cfg.wateringTimesCount = MAX_WATERING_SLOTS;
  if (!(cfg.relay.activeLevel == "LOW" || cfg.relay.activeLevel == "HIGH")) cfg.relay.activeLevel = "LOW";
  if (cfg.waterLevel.debounceSamples < 1) cfg.waterLevel.debounceSamples = 1;
  if (cfg.waterLevel.debounceSamples > MAX_DEBOUNCE_SAMPLES) cfg.waterLevel.debounceSamples = MAX_DEBOUNCE_SAMPLES;
  if (cfg.waterLevel.debounceIntervalMs < 1) cfg.waterLevel.debounceIntervalMs = 1;
  if (cfg.waterLevel.debounceIntervalMs > 1000) cfg.waterLevel.debounceIntervalMs = 1000;
  
  if (cfg.bme280.tempMin > cfg.bme280.tempMax) {
    float temp = cfg.bme280.tempMin;
    cfg.bme280.tempMin = cfg.bme280.tempMax;
    cfg.bme280.tempMax = temp;
  }
  if (cfg.bme280.humMin > cfg.bme280.humMax) {
    float hum = cfg.bme280.humMin;
    cfg.bme280.humMin = cfg.bme280.humMax;
    cfg.bme280.humMax = hum;
  }
  if (cfg.bme280.presMin > cfg.bme280.presMax) {
    float pres = cfg.bme280.presMin;
    cfg.bme280.presMin = cfg.bme280.presMax;
    cfg.bme280.presMax = pres;
  }
}

bool loadConfigurationFromFile() {
  if (!LittleFS.exists("/config.json")) {
    Serial.println("config.json not found.");
    return false;
  }
  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Could not open config.json for reading.");
    return false;
  }
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return false;
  }
  applyConfigFromJson(doc, currentConfig, nullptr);
  validateAndClampConfig(currentConfig);
  Serial.println("Configuration successfully loaded from /config.json.");
  return true;
}

void saveConfigurationToFile() {
  Serial.println("Saving configuration to /config.json (atomic)...");
  const char *tmpPath = "/config.json.tmp";
  const char *dstPath = "/config.json";
  File tmp = LittleFS.open(tmpPath, "w");
  if (!tmp) {
    Serial.println("Failed to create temp config file for writing.");
    return;
  }
  StaticJsonDocument<2048> doc;
  configToJson(doc, currentConfig, true);
  if (serializeJson(doc, tmp) == 0) {
    Serial.println(F("Failed to write JSON to temp config"));
    tmp.close();
    LittleFS.remove(tmpPath);
    return;
  }
  tmp.flush();
  tmp.close();
  if (LittleFS.exists(dstPath)) LittleFS.remove(dstPath);
  if (!LittleFS.rename(tmpPath, dstPath)) {
    Serial.println("Failed to rename temp config to /config.json");
  } else {
    Serial.println(F("Configuration successfully saved to /config.json."));
  }
}

AsyncWebServer server(80);
RTC_DS3231 rtc;
Adafruit_BME280 bme;

unsigned long lastSensorReadTime = 0;
unsigned long lastWaterLevelSampleTime = 0;
int waterLevelReadings[10];
int waterLevelReadingIndex = 0;
int stableWaterLevelState = -1;
bool bmeSuccessfullyInitialized = false;

void setup() {
  Serial.begin(115200);
  Serial.println("\nLaistymo sistemos paleidimas...");

  if (!LittleFS.begin(true)) {
    Serial.println("Klaida montuojant LittleFS! Net po bandymo formatuoti.");
    while (1) delay(1000);
  } else {
    Serial.println("LittleFS prijungta sekmingai.");
  }

  if (!loadConfigurationFromFile()) {
    Serial.println("Nepavyko užkrauti konfigūracijos iš failo arba failas nerastas.");
    Serial.println("Naudojama numatytoji konfigūracija ir bandoma ją išsaugoti.");
    loadDefaultConfig();
    saveConfigurationToFile();
  }

  Serial.print("Kuriama SoftAP prieiga: ");
  Serial.println(currentConfig.wifi.apSsid);
  WiFi.softAP(currentConfig.wifi.apSsid.c_str(), 
              currentConfig.wifi.apPassword.c_str(), 
              currentConfig.wifi.apChannel, 
              currentConfig.wifi.apHidden);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP adresas: ");
  Serial.println(IP);

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("Nepavyko rasti RTC modulio! Patikrinkite pajungimą.");
  } else {
    if (rtc.lostPower()) {
      Serial.println("RTC prarado maitinimą, nustatykite laiką!");
      DateTime cfgTime;
      if (parseIsoDateTime(currentConfig.time, cfgTime)) {
        rtc.adjust(cfgTime);
        Serial.println("RTC sureguliuotas pagal config.time");
        currentDateTime = cfgTime;
      }
    }
    Serial.println("RTC modulis rastas.");
    DateTime now = rtc.now();
    if (now.isValid()) {
      currentDateTime = now;
    }
  }
  
  bool bmeFound = false;
  if (bme.begin(0x76)) {
    Serial.println("BME280 modulis rastas (adresas 0x76).");
    bmeFound = true;
  } else if (bme.begin(0x77)) {
    Serial.println("BME280 modulis rastas (adresas 0x77).");
    bmeFound = true;
  }
  if (!bmeFound) {
    Serial.println("Nepavyko rasti BME280 modulio! Patikrinkite pajungimą ir adresą (0x76 arba 0x77).");
  } else {
    bmeSuccessfullyInitialized = true;
  }

  if (currentConfig.waterLevel.pullMode == "PULLUP") {
    pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
    Serial.println("Water level sensor PIN " + String(WATER_LEVEL_PIN) + " initialized with PULLUP.");
  } else if (currentConfig.waterLevel.pullMode == "PULLDOWN") {
    pinMode(WATER_LEVEL_PIN, INPUT_PULLDOWN);
    Serial.println("Water level sensor PIN " + String(WATER_LEVEL_PIN) + " initialized with PULLDOWN.");
  } else {
    pinMode(WATER_LEVEL_PIN, INPUT);
    Serial.println("Water level sensor PIN " + String(WATER_LEVEL_PIN) + " initialized as INPUT (no pull resistor defined in config).");
  }
  validateAndClampConfig(currentConfig);
  int initSamples = getEffectiveDebounceSamples();
  for (int i = 0; i < initSamples; i++) {
    waterLevelReadings[i] = digitalRead(WATER_LEVEL_PIN);
  }
  waterLevelReadingIndex = 0;
  stableWaterLevelState = waterLevelReadings[0];
  currentWaterLevelState = (stableWaterLevelState == (currentConfig.waterLevel.minState == "HIGH" ? HIGH : LOW)) ? "NERA" : "YRA";
  Serial.print("Water level changed to: "); Serial.println(currentWaterLevelState);

  if (!(currentConfig.relay.activeLevel == "LOW" || currentConfig.relay.activeLevel == "HIGH")) {
    currentConfig.relay.activeLevel = "LOW";
  }
  relayActiveLow = (currentConfig.relay.activeLevel == "LOW");
  pinMode(RELAY_PIN, OUTPUT);
  turnRelayOff();
  relayIsOn = false;

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/ui/index.html")) {
      request->send(LittleFS, "/ui/index.html", "text/html");
    } else {
      request->send(404, "text/plain", "UI file /ui/index.html not found in LittleFS");
    }
  });
  
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(512);
    doc["temp"] = currentTemperature;
    doc["hum"] = currentHumidity;
    doc["pres"] = currentPressure;
    doc["waterLevel"] = currentWaterLevelState;
    doc["state"] = systemState;
    doc["remainingTimeSec"] = remainingWateringTimeSec;
    char isoTime[20];
    sprintf(isoTime, "%04d-%02d-%02dT%02d:%02d:%02d", 
            currentDateTime.year(), currentDateTime.month(), currentDateTime.day(), 
            currentDateTime.hour(), currentDateTime.minute(), currentDateTime.second());
    doc["currentTime"] = isoTime;

    long nextStartInSec = -1;
    if (currentDateTime.isValid() && currentConfig.wateringTimesCount > 0) {
      DateTime best;
      bool foundToday = false;
      for (int i = 0; i < currentConfig.wateringTimesCount; i++) {
        String s = currentConfig.wateringTimes[i];
        if (s.length() != 5 || s.charAt(2) != ':') continue;
        int th = s.substring(0,2).toInt();
        int tm = s.substring(3,5).toInt();
        DateTime cand(currentDateTime.year(), currentDateTime.month(), currentDateTime.day(), th, tm, 0);
        if (cand >= currentDateTime) {
          if (!foundToday || cand < best) { best = cand; foundToday = true; }
        }
      }
      if (!foundToday) {
        int minh = 23, minm = 59; bool have = false;
        for (int i = 0; i < currentConfig.wateringTimesCount; i++) {
          String s = currentConfig.wateringTimes[i];
          if (s.length() != 5 || s.charAt(2) != ':') continue;
          int th = s.substring(0,2).toInt();
          int tm = s.substring(3,5).toInt();
          if (!have || th < minh || (th == minh && tm < minm)) { minh = th; minm = tm; have = true; }
        }
        if (have) {
          DateTime tomorrow = currentDateTime + TimeSpan(1,0,0,0);
          best = DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), minh, minm, 0);
        }
      }
      if (best.isValid()) {
        TimeSpan delta = best - currentDateTime;
        nextStartInSec = (long)delta.totalseconds();
        if (nextStartInSec < 0) nextStartInSec = 0;
      }
    }
    doc["nextStartInSec"] = nextStartInSec;

    long windowRemainingSec = 0;
    if (currentDateTime.isValid() && (currentState == STATE_WINDOW_OPEN || currentState == STATE_WATERING || currentState == STATE_ERROR_PAUSED)) {
      if (currentDateTime < windowEndsAt) {
        TimeSpan rem = windowEndsAt - currentDateTime;
        windowRemainingSec = (long)rem.totalseconds();
        if (windowRemainingSec < 0) windowRemainingSec = 0;
      }
    }
    doc["windowRemainingSec"] = windowRemainingSec;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<2048> doc;
    configToJson(doc, currentConfig, false);
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  server.on("/config", HTTP_POST,
    [](AsyncWebServerRequest *request){ },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0) {
        request->_tempObject = new String();
      }
      String *body = reinterpret_cast<String*>(request->_tempObject);
      body->reserve(total);
      body->concat((const char*)data, len);

      if (index + len == total) {
        String postBody = *body;
        delete body;
        request->_tempObject = nullptr;

        Serial.println("Received POST /config body: " + postBody);
        StaticJsonDocument<2048> doc;
        DeserializationError error = deserializeJson(doc, postBody);
        if (error) {
          Serial.print(F("deserializeJson() failed for POST /config: "));
          Serial.println(error.f_str());
          request->send(400, "application/json", "{\"error\":\"Invalid JSON format\"}");
          return;
        }

        bool wifiChanged = false;
        int prevDebounceSamples = currentConfig.waterLevel.debounceSamples;
        int prevDebounceInterval = currentConfig.waterLevel.debounceIntervalMs;
        String prevTime = currentConfig.time;
        applyConfigFromJson(doc, currentConfig, &wifiChanged);
        validateAndClampConfig(currentConfig);
        bool timeChanged = (currentConfig.time != prevTime);
        if (wifiChanged) {
          Serial.println("Wi-Fi settings changed. Restart required to apply new SSID/password.");
        }

        if (timeChanged) {
          DateTime newTime;
          if (parseIsoDateTime(currentConfig.time, newTime)) {
            rtc.adjust(newTime);
            currentDateTime = newTime;
            Serial.println("RTC time set from POST /config time field.");
          } else {
            Serial.println("Gautas 'time' laukas neatitinka formato, RTC nebuvo atnaujintas.");
          }
        }

        saveConfigurationToFile();

        if (currentConfig.waterLevel.debounceSamples != prevDebounceSamples || currentConfig.waterLevel.debounceIntervalMs != prevDebounceInterval) {
          int eff = getEffectiveDebounceSamples();
          for (int i = 0; i < eff; i++) waterLevelReadings[i] = digitalRead(WATER_LEVEL_PIN);
          waterLevelReadingIndex = 0;
          stableWaterLevelState = waterLevelReadings[0];
          currentWaterLevelState = (stableWaterLevelState == (currentConfig.waterLevel.minState == "HIGH" ? HIGH : LOW)) ? "NERA" : "YRA";
          lastWaterLevelSampleTime = millis();
        }
        request->send(200, "application/json", "{\"success\":\"Configuration updated\"}");
      }
    }
  );

  server.on("/config/time", HTTP_POST,
    [](AsyncWebServerRequest *request){ },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0) {
        request->_tempObject = new String();
      }
      String *body = reinterpret_cast<String*>(request->_tempObject);
      body->reserve(total);
      body->concat((const char*)data, len);

      if (index + len == total) {
        String postBody = *body;
        delete body;
        request->_tempObject = nullptr;

        Serial.println("Received POST /config/time body: " + postBody);
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, postBody);

        if (error || !doc.containsKey("time") || !doc["time"].is<const char*>()) {
          Serial.print(F("deserializeJson() failed for POST /config/time or 'time' key missing/invalid: "));
          if(error) Serial.println(error.f_str()); else Serial.println("Missing 'time' key or invalid type");
          request->send(400, "application/json", "{\"error\":\"Invalid JSON format or missing 'time' key (expected string YYYY-MM-DDTHH:MM:SS)\"}");
          return;
        }

        String timeStr = doc["time"].as<String>();
        DateTime newTime;
        if (!parseIsoDateTime(timeStr, newTime)) {
          request->send(400, "application/json", "{\"error\":\"Invalid time string format or values. Expected YYYY-MM-DDTHH:MM:SS\"}");
          return;
        }

        rtc.adjust(newTime);
        currentConfig.time = timeStr;
        saveConfigurationToFile();

        Serial.println("RTC time set to: " + timeStr);
        request->send(200, "application/json", "{\"success\":\"Time updated\", \"newTime\":\"" + timeStr + "\"}");
      }
    }
  );

  server.on("/start", HTTP_POST, [](AsyncWebServerRequest *request){
    if (currentState == STATE_WATERING) {
      request->send(409, "application/json", "{\"error\":\"Watering already in progress\"}");
      return;
    }
    if (!checkWateringConditions()) {
      request->send(409, "application/json", "{\"error\":\"Conditions not met for watering\"}");
      return;
    }

    setState(STATE_WATERING);
    remainingWateringTimeSec = currentConfig.wateringDurationMin * 60;
    isManualWatering = true;
    Serial.println("Manual watering started. Duration: " + String(currentConfig.wateringDurationMin) + " min.");
    
    StaticJsonDocument<128> doc;
    doc["status"] = "Watering started";
    doc["durationMin"] = currentConfig.wateringDurationMin;
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  server.on("/stop", HTTP_POST, [](AsyncWebServerRequest *request){
    if (currentState != STATE_WATERING) {
      request->send(409, "application/json", "{\"error\":\"No watering cycle in progress to stop\"}");
      return;
    }

    setState(STATE_IDLE);
    remainingWateringTimeSec = 0;
    isManualWatering = false;
    Serial.println("Manual watering stopped.");

    StaticJsonDocument<64> doc;
    doc["status"] = "Watering stopped";
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", "{\"success\":\"Restarting\"}");
    delay(100);
    ESP.restart();
  });

  server.serveStatic("/ui", LittleFS, "/ui/").setDefaultFile("index.html");

  server.begin();
  Serial.println("HTTP serveris paleistas.");
}

void loop() {
  static unsigned long lastRtcReadTime = 0;
  if (millis() - lastRtcReadTime >= 1000) {
    lastRtcReadTime = millis();
    if (rtc.now().isValid()) {
        currentDateTime = rtc.now();
    } else {
        Serial.println("RTC laikas nevalidus!");
    }
  }

  if (millis() - lastSensorReadTime >= (unsigned long)currentConfig.sensorReadIntervalMs) {
    lastSensorReadTime = millis();

    if (bmeSuccessfullyInitialized) {
      currentTemperature = bme.readTemperature();
      currentHumidity = bme.readHumidity();
      currentPressure = bme.readPressure() / 100.0F;
    } else {
      currentTemperature = -999.0;
      currentHumidity = -999.0;
      currentPressure = -999.0;
    }
  }

  if (millis() - lastWaterLevelSampleTime >= (unsigned long)currentConfig.waterLevel.debounceIntervalMs) {
    lastWaterLevelSampleTime = millis();
    int effectiveSamples = getEffectiveDebounceSamples();
    waterLevelReadings[waterLevelReadingIndex] = digitalRead(WATER_LEVEL_PIN);
    waterLevelReadingIndex = (waterLevelReadingIndex + 1) % effectiveSamples;

    bool allSame = true;
    for (int i = 0; i < effectiveSamples - 1; i++) {
      if (waterLevelReadings[i] != waterLevelReadings[i+1]) { allSame = false; break; }
    }
    if (allSame) {
      if (stableWaterLevelState != waterLevelReadings[0]) {
        stableWaterLevelState = waterLevelReadings[0];
        bool isMinLevel = (currentConfig.waterLevel.minState == "HIGH" && stableWaterLevelState == HIGH) ||
                          (currentConfig.waterLevel.minState == "LOW" && stableWaterLevelState == LOW);
        currentWaterLevelState = isMinLevel ? "NERA" : "YRA";
        Serial.print("Water level changed to: "); Serial.println(currentWaterLevelState);
      }
    }
  }

  switch (currentState) {
    case STATE_IDLE: {
      if (currentDateTime.isValid()) {
        int todayYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
        static bool slotsInited = false;
        if (!slotsInited) {
          for (int i = 0; i < MAX_WATERING_SLOTS; i++) lastWateringYMDForSlot[i] = -1;
          slotsInited = true;
        }
        for (int i = 0; i < currentConfig.wateringTimesCount; i++) {
          String s = currentConfig.wateringTimes[i];
          if (s.length() != 5 || s.charAt(2) != ':') continue;
          int targetHour = s.substring(0, 2).toInt();
          int targetMinute = s.substring(3, 5).toInt();
          DateTime scheduled(currentDateTime.year(), currentDateTime.month(), currentDateTime.day(), targetHour, targetMinute, 0);
          TimeSpan tol(0, 0, currentConfig.toleranceWindowMin, 0);
          bool inWindow = (currentDateTime >= scheduled) && (currentDateTime < (scheduled + tol));
      if (inWindow && lastWateringYMDForSlot[i] != todayYMD) {
            setState(STATE_WINDOW_OPEN);
            activeSlotIndex = i;
            windowEndsAt = scheduled + tol;
            Serial.print("State changed to: WindowOpen (slot "); Serial.print(i); Serial.println(")");
            Serial.print("Window ends at: ");
            char isoTime[20];
            sprintf(isoTime, "%04d-%02d-%02dT%02d:%02d:%02d",
                    windowEndsAt.year(), windowEndsAt.month(), windowEndsAt.day(),
                    windowEndsAt.hour(), windowEndsAt.minute(), windowEndsAt.second());
            Serial.println(isoTime);
            break;
          }
        }
      }
      break;
    }
    case STATE_WINDOW_OPEN: {
      if (currentDateTime.isValid()) {
        if (currentDateTime >= windowEndsAt) {
          Serial.println("Watering window closed. No watering initiated or finished.");
          setState(STATE_IDLE);
          activeSlotIndex = -1;
          isManualWatering = false;
          break;
        }
      }
      
      bool conditionsOk = checkWateringConditions();
      if (conditionsOk) {
        setState(STATE_WATERING);
        remainingWateringTimeSec = currentConfig.wateringDurationMin * 60;
        isManualWatering = false;
        Serial.println("Conditions OK. State changed to: Watering");
        if (currentDateTime.isValid()) {
          int todayYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
          if (activeSlotIndex >= 0 && activeSlotIndex < MAX_WATERING_SLOTS) {
            lastWateringYMDForSlot[activeSlotIndex] = todayYMD;
          }
        }
      }
      break;
    }
    case STATE_WATERING: {
      if (!isManualWatering && currentDateTime.isValid() && currentDateTime >= windowEndsAt) {
        Serial.println("Watering window closed during automatic watering. Stopping.");
        setState(STATE_IDLE);
        activeSlotIndex = -1;
        remainingWateringTimeSec = 0;
        isManualWatering = false;
        break;
      }
      
      static unsigned long lastWateringSecondTick = 0;

      if (millis() - lastWateringSecondTick >= 1000) {
        lastWateringSecondTick = millis();
        if (remainingWateringTimeSec > 0) {
          remainingWateringTimeSec--;
          if (!checkWateringConditions()) {
            Serial.println("Conditions became invalid during watering. Pausing with error.");
            setState(STATE_ERROR_PAUSED);
          }
        }       
      }

      if (remainingWateringTimeSec == 0) {
        if (isManualWatering) {
          Serial.println("Manual watering finished by timer.");
        } else {
          Serial.println("Automatic watering finished by timer.");
        }
        setState(STATE_IDLE);
        activeSlotIndex = -1;
        isManualWatering = false;
      }
      break;
    }
    case STATE_ERROR_PAUSED: {
      if (!isManualWatering && currentDateTime.isValid() && currentDateTime >= windowEndsAt) {
        Serial.println("Watering window closed during error pause. Returning to Idle.");
        setState(STATE_IDLE);
        activeSlotIndex = -1;
        remainingWateringTimeSec = 0;
        isManualWatering = false;
        break;
      }
      
      static unsigned long lastErrorCheck = 0;
      if (millis() - lastErrorCheck >= (unsigned long)currentConfig.pauseResumeCheckIntervalMs) {
        lastErrorCheck = millis();
        if (checkWateringConditions()) {
          if (currentDateTime.isValid() && currentDateTime < windowEndsAt && remainingWateringTimeSec > 0) {
            Serial.println("Error conditions cleared. Resuming watering.");
            setState(STATE_WATERING);
          } else {
            Serial.println("Error conditions cleared but window closed or watering time finished. Returning to Idle.");
            setState(STATE_IDLE);
            activeSlotIndex = -1;
            remainingWateringTimeSec = 0;
            isManualWatering = false;
          }
        }
      }
      break;
    }
    default:
      Serial.println("Unknown system state! Returning to Idle.");
      setState(STATE_IDLE);
      isManualWatering = false;
      break;
  }
}

bool checkWateringConditions() {
  static int lastStatus = 0;
  int status = 0;
  bool ok = true;

  if (currentWaterLevelState == "NERA") {
    status = 2; ok = false;
  }

  if (ok && bmeSuccessfullyInitialized) {
    if (currentTemperature < currentConfig.bme280.tempMin || currentTemperature > currentConfig.bme280.tempMax) {
      status = 3; ok = false;
    } else if (currentHumidity < currentConfig.bme280.humMin || currentHumidity > currentConfig.bme280.humMax) {
      status = 4; ok = false;
    } else if (currentPressure < currentConfig.bme280.presMin || currentPressure > currentConfig.bme280.presMax) {
      status = 5; ok = false;
    } else {
      status = 1;
    }
  } else if (ok && !bmeSuccessfullyInitialized) {
    bool bmeLimitsSet = (currentConfig.bme280.tempMin != 0.0 || currentConfig.bme280.tempMax != 0.0 ||
                         currentConfig.bme280.humMin != 0.0 || currentConfig.bme280.humMax != 0.0 ||
                         currentConfig.bme280.presMin != 0.0 || currentConfig.bme280.presMax != 0.0);
    if (bmeLimitsSet) { status = 6; ok = false; } else { status = 7; ok = true; }
  }

  if (status != lastStatus) {
    lastStatus = status;
    switch (status) {
      case 1: Serial.println("Condition: OK"); break;
      case 2: Serial.println("Condition: FAIL (water level MIN)"); break;
      case 3: Serial.println("Condition: FAIL (temperature)"); break;
      case 4: Serial.println("Condition: FAIL (humidity)"); break;
      case 5: Serial.println("Condition: FAIL (pressure)"); break;
      case 6: Serial.println("Condition: FAIL (BME not initialized, limits set)"); break;
      case 7: Serial.println("Condition: OK (BME skipped)"); break;
      default: break;
    }
  }

  return ok;
}

