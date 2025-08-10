#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "RTClib.h" // RTC biblioteka
#include <Adafruit_BME280.h> // BME280 biblioteka
#include <Adafruit_Sensor.h> // Būtina Adafruit jutiklių bibliotekoms

// !!! SVARBU: Pakeiskite WATER_LEVEL_PIN į kontaktą, kurį naudojate vandens lygio jutikliui !!!
#define WATER_LEVEL_PIN 13 // Pavyzdys, pakeiskite pagal savo schemą
// !!! SVARBU: Pakeiskite RELAY_PIN į kontaktą, kuriuo valdote relę (siurblį/vožtuvą) !!!
#define RELAY_PIN 12 // Pavyzdys, pakeiskite pagal savo schemą

// Maksimalus debouncing imčių kiekis
#define MAX_DEBOUNCE_SAMPLES 10

// Forward deklaracija, kad būtų galima naudoti setup() anksčiau
bool parseIsoDateTime(const String &iso, DateTime &out);

// --- Būsenų enum ir konvertavimo funkcijos (perkeltos aukščiau, kad būtų prieinamos visur) ---
enum SystemStateEnum {
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

// --- Konfigūracijos struktūra ---
struct WaterLevelConfig {
  String minState;          // "HIGH" arba "LOW"
  int debounceSamples;
  int debounceIntervalMs;
  String pullMode;          // "PULLUP" arba "PULLDOWN"
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
  String activeLevel;     // "LOW" arba "HIGH"; nurodo, koks lygis įjungia relę
};

struct Config {
  String time; // Laikoma kaip ISO stringas, konvertuojama į/iš RTC DateTime
  int wateringDurationMin;
  int toleranceWindowMin;
  int sensorReadIntervalMs;
  int pauseResumeCheckIntervalMs;
  WaterLevelConfig waterLevel;
  BME280Config bme280;
  WifiConfig wifi;
  RelayConfig relay;
};

Config currentConfig; // Globalus konfigūracijos objektas

// Pagalbinė funkcija gauti saugų debouncing imčių skaičių (po Config deklaracijos)
static inline int getEffectiveDebounceSamples() {
  int s = currentConfig.waterLevel.debounceSamples;
  if (s < 1) return 1;
  if (s > MAX_DEBOUNCE_SAMPLES) return MAX_DEBOUNCE_SAMPLES;
  return s;
}

// -- Config (de)serializacijos helper'iai --
static inline void configToJson(JsonDocument &doc, const Config &cfg) {
  doc["time"] = cfg.time;
  doc["wateringDurationMin"] = cfg.wateringDurationMin;
  doc["toleranceWindowMin"] = cfg.toleranceWindowMin;
  doc["sensorReadIntervalMs"] = cfg.sensorReadIntervalMs;
  doc["pauseResumeCheckIntervalMs"] = cfg.pauseResumeCheckIntervalMs;

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
}

static inline bool applyConfigFromJson(const JsonDocument &doc, Config &cfg, bool *outWifiChanged = nullptr) {
  bool wifiChanged = false;
  if (doc.containsKey("time")) cfg.time = doc["time"].as<String>();
  if (doc.containsKey("wateringDurationMin")) cfg.wateringDurationMin = doc["wateringDurationMin"];
  if (doc.containsKey("toleranceWindowMin")) cfg.toleranceWindowMin = doc["toleranceWindowMin"];
  if (doc.containsKey("sensorReadIntervalMs")) cfg.sensorReadIntervalMs = doc["sensorReadIntervalMs"];
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
    if (wifiDoc.containsKey("apChannel")) cfg.wifi.apChannel = wifiDoc["apChannel"];
    if (wifiDoc.containsKey("apHidden")) cfg.wifi.apHidden = wifiDoc["apHidden"];
  }

  JsonObjectConst relayDoc = doc["relay"].as<JsonObjectConst>();
  if (!relayDoc.isNull()) {
    if (relayDoc.containsKey("activeLevel")) cfg.relay.activeLevel = relayDoc["activeLevel"].as<String>();
  }

  if (outWifiChanged) *outWifiChanged = wifiChanged;
  return true;
}

// --- Globalūs būsenos kintamieji (bus atnaujinami vėliau realiais duomenimis) ---
float currentTemperature = -999.0; // Laipsniai Celsijaus
float currentHumidity = -999.0;    // Procentai %
float currentPressure = -999.0;    // hPa
String currentWaterLevelState = "UNKNOWN"; // Pvz., "HIGH", "LOW", pagal config.waterLevel.minState
SystemStateEnum currentState = STATE_IDLE; // enum pagrindinė būsena
String systemState = systemStateToString(currentState); // String būsena, sinchronizuota su enum
unsigned int remainingWateringTimeSec = 0; // Likęs laistymo laikas sekundėmis
DateTime currentDateTime; // Globalus laiko objektas
DateTime windowEndsAt;    // Kada baigiasi dabartinis laistymo langas
int lastWateringYMD = -1; // YYYYMMDD, kad nevykdyti daugiau nei kartą per dieną

// Relės būsena ir pagalbinės funkcijos
bool relayActiveLow = true; // nustatoma setup() pagal config
bool relayIsOn = false;     // optimizacijai: rašome į PIN tik keičiant būseną
inline void turnRelayOn() {
  digitalWrite(RELAY_PIN, relayActiveLow ? LOW : HIGH);
}
inline void turnRelayOff() {
  digitalWrite(RELAY_PIN, relayActiveLow ? HIGH : LOW);
}

// Centralizuota būsena su relės valdymu ir String sinchronizacija
static inline void setState(SystemStateEnum newState) {
  if (newState == currentState) return;
  // Valdyti relę pagal būseną
  if (newState == STATE_WATERING) {
    if (!relayIsOn) { turnRelayOn(); relayIsOn = true; }
  } else {
    if (relayIsOn) { turnRelayOff(); relayIsOn = false; }
  }
  currentState = newState;
  systemState = systemStateToString(currentState);
}

// --- Numatytosios konfigūracijos reikšmės (pagal jūsų dokumentaciją) ---
void loadDefaultConfig() {
  currentConfig.time = "2025-05-08T08:00:00";
  currentConfig.wateringDurationMin = 60;
  currentConfig.toleranceWindowMin = 120;
  currentConfig.sensorReadIntervalMs = 1000;
  currentConfig.pauseResumeCheckIntervalMs = 1000;

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
  // Relė pagal nutylėjimą aktyvi LOW (dauguma modulių); pinas – #define RELAY_PIN
  currentConfig.relay.activeLevel = "LOW";
  
  Serial.println("Loaded default configuration.");
}

// --- Failų sistemos ir konfigūracijos funkcijos ---
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
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return false;
  }
  applyConfigFromJson(doc, currentConfig, nullptr);
  Serial.println("Configuration successfully loaded from /config.json.");
  return true;
}

void saveConfigurationToFile() {
  Serial.println("Saving configuration to /config.json...");
  if (LittleFS.exists("/config.json")) {
    LittleFS.remove("/config.json");
  }
  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to create config.json for writing.");
    return;
  }
  StaticJsonDocument<1024> doc;
  configToJson(doc, currentConfig);
  if (serializeJson(doc, configFile) == 0) {
    Serial.println(F("Failed to write to config.json"));
  } else {
    Serial.println(F("Configuration successfully saved to /config.json."));
  }
  configFile.close();
}

// --- Globalūs objektai ---
AsyncWebServer server(80); // Web serveris ant 80 porto
RTC_DS3231 rtc;           // RTC objektas (DS3231)
Adafruit_BME280 bme;      // BME280 objektas (I2C)

// Kintamieji jutiklių nuskaitymui ir debouncing
unsigned long lastSensorReadTime = 0;
int waterLevelReadings[10]; // Masyvas debouncing'ui, dydis pagal max debounceSamples
int waterLevelReadingIndex = 0;
int stableWaterLevelState = -1; // -1 reiškia neapsispręsta, 0 LOW, 1 HIGH
bool bmeSuccessfullyInitialized = false;

// --- setup() funkcija ---
void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // laukti, kol prisijungs Serial Monitor
  }
  Serial.println("\nLaistymo sistemos paleidimas...");

  // 1. Inicializuoti LittleFS
  if (!LittleFS.begin(true)) { // Pakeista čia: pridėtas 'true' argumentas formatavimui
    Serial.println("Klaida montuojant LittleFS! Net po bandymo formatuoti.");
    // Galima bandyti sustabdyti arba naudoti labai griežtas numatytąsias reikšmes.
    // Bet sistema negalės veikti be konfigūracijos.
    // Galima sustabdyti arba naudoti labai griežtas numatytąsias reikšmes.
    while (1) delay(1000); // Sustoti, jei LittleFS nepavyko
  } else {
    Serial.println("LittleFS prijungta sekmingai.");
  }

  // 2. Užkrauti konfigūraciją
  if (!loadConfigurationFromFile()) {
    Serial.println("Nepavyko užkrauti konfigūracijos iš failo arba failas nerastas.");
    Serial.println("Naudojama numatytoji konfigūracija ir bandoma ją išsaugoti.");
    loadDefaultConfig();
    saveConfigurationToFile(); // Išsaugome numatytąją konfigūraciją, kad ji būtų kitam kartui
  }
  // Jei loadConfigurationFromFile() pavyko, pranešimas apie sėkmingą užkrovimą bus pačioje funkcijoje.

  // 3. Inicializuoti Wi-Fi SoftAP
  Serial.print("Kuriama SoftAP prieiga: ");
  Serial.println(currentConfig.wifi.apSsid);
  WiFi.softAP(currentConfig.wifi.apSsid.c_str(), 
              currentConfig.wifi.apPassword.c_str(), 
              currentConfig.wifi.apChannel, 
              currentConfig.wifi.apHidden);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP adresas: ");
  Serial.println(IP);

  // 4. Inicializuoti RTC
  if (!rtc.begin()) {
    Serial.println("Nepavyko rasti RTC modulio! Patikrinkite pajungimą.");
    // Galima bandyti veikti be RTC, bet laiko funkcijos neveiks.
    // Arba sustoti, priklausomai nuo reikalavimų.
  } else {
    if (rtc.lostPower()) {
      Serial.println("RTC prarado maitinimą, nustatykite laiką!");
      // Laikinai, kol nėra UI nustatymo, galima naudoti kompiliavimo laiką
      // rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); 
      // Arba laikas bus nustatytas per /config/time POST, pagal dokumentaciją
      // Jei config'e yra teisingas ISO laikas, pabandome sureguliuoti RTC
      DateTime cfgTime;
      if (parseIsoDateTime(currentConfig.time, cfgTime)) {
        rtc.adjust(cfgTime);
        Serial.println("RTC sureguliuotas pagal config.time");
      }
    }
    Serial.println("RTC modulis rastas.");
    // TODO: Nustatyti laiką iš currentConfig.time
  }
  
  // 5. Inicializuoti BME280
  // Bandome abu populiarius adresus: 0x76 ir 0x77
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
    // Sistema gali veikti su apribojimais arba sustoti.
  } else {
    bmeSuccessfullyInitialized = true;
  }

  // Inicializuoti vandens lygio jutiklio kontaktą
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
  // Užpildyti pradinius debouncing masyvo įrašus
  for (int i = 0; i < currentConfig.waterLevel.debounceSamples; i++) {
    waterLevelReadings[i] = digitalRead(WATER_LEVEL_PIN);
  }
  stableWaterLevelState = digitalRead(WATER_LEVEL_PIN);
  currentWaterLevelState = (stableWaterLevelState == (currentConfig.waterLevel.minState == "HIGH" ? HIGH : LOW)) ? "MIN" : "OK"; // Arba "MAX", "ABOVE_MIN" ir pan.
  Serial.print("Water level changed to: "); Serial.println(currentWaterLevelState);

  // Apsauga: apriboti debounceSamples į leistinį intervalą
  if (currentConfig.waterLevel.debounceSamples < 1) {
    Serial.println("debounceSamples < 1 -> nustatoma į 1");
    currentConfig.waterLevel.debounceSamples = 1;
  }
  if (currentConfig.waterLevel.debounceSamples > MAX_DEBOUNCE_SAMPLES) {
    Serial.println("debounceSamples viršijo MAX -> apkarpoma iki MAX");
    currentConfig.waterLevel.debounceSamples = MAX_DEBOUNCE_SAMPLES;
  }

  // Inicializuoti relę
  // Jei config.relay.activeLevel neapibrėžtas, laikome LOW kaip saugią numatytąją
  if (!(currentConfig.relay.activeLevel == "LOW" || currentConfig.relay.activeLevel == "HIGH")) {
    currentConfig.relay.activeLevel = "LOW";
  }
  relayActiveLow = (currentConfig.relay.activeLevel == "LOW");
  pinMode(RELAY_PIN, OUTPUT);
  turnRelayOff(); // Saugiai išjungta paleidimo metu
  relayIsOn = false;

  // 6. Web serverio maršrutai (pradinis pavyzdys)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>Laistymo Sistema</title><meta charset=\"UTF-8\"></head><body>"; // Pridėta meta žymė
    html += "<h1>Laistymo Sistema veikia!</h1>";
    html += "<p>SSID: " + currentConfig.wifi.apSsid + "</p>";
    html += "<p>AP IP: " + WiFi.softAPIP().toString() + "</p>";
    html += "<p><a href='/status'>Būsena</a></p>";
    html += "<p><a href='/config'>Konfigūracija (JSON)</a></p>";
    html += "<p><a href='/ui/index.html'>Vartotojo Sąsaja (jei įkelta)</a></p>";
    html += "</body></html>";
    request->send(200, "text/html", html);
  });
  
  // GET /status endpoint'as
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(512); // Padidinta, kad tilptų laikas
    doc["temp"] = currentTemperature;
    doc["hum"] = currentHumidity;
    doc["pres"] = currentPressure;
    doc["waterLevel"] = currentWaterLevelState;
    doc["state"] = systemState;
    doc["remainingTimeSec"] = remainingWateringTimeSec;
    // Pridedame RTC laiką YYYY-MM-DDTHH:MM:SS formatu
    char isoTime[20];
    sprintf(isoTime, "%04d-%02d-%02dT%02d:%02d:%02d", 
            currentDateTime.year(), currentDateTime.month(), currentDateTime.day(), 
            currentDateTime.hour(), currentDateTime.minute(), currentDateTime.second());
    doc["currentTime"] = isoTime;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  // GET /config endpoint'as
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<1024> doc; // Dydis turi atitikti save/load funkcijų dydį
    configToJson(doc, currentConfig);
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  // POST /config endpoint'as konfigūracijos atnaujinimui
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL, // Nėra failo įkėlimo handler'io
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      // Ši funkcija kviečiama, kai gaunama POST body dalis
      // Kadangi tikimės JSON, sujungsime visas dalis į vieną String
      // Paprastumo dėlei, darome prielaidą, kad visas JSON telpa į atmintį.
      // Didesniems JSON reikėtų sudėtingesnio apdorojimo.
      String postBody = "";
      if(total == 0) { // Jei body tuščias
        request->send(400, "application/json", "{\"error\":\"Empty request body\"}");
        return;
      }
      for(size_t i=0; i<len; i++){
        postBody += (char)data[i];
      }

      if(index + len == total){ // Gautas visas body
        Serial.println("Received POST /config body: " + postBody);
        StaticJsonDocument<1024> doc; // Dydis turi atitikti load/save funkcijų dydį
        DeserializationError error = deserializeJson(doc, postBody);

        if (error) {
          Serial.print(F("deserializeJson() failed for POST /config: "));
          Serial.println(error.f_str());
          request->send(400, "application/json", "{\"error\":\"Invalid JSON format\"}");
          return;
        }

        bool wifiChanged = false;
        applyConfigFromJson(doc, currentConfig, &wifiChanged);
        if (wifiChanged) {
          Serial.println("Wi-Fi settings changed. Restart required to apply new SSID/password.");
          // TODO: Galima pridėti vėliavą ar mechanizmą, kuris UI praneštų apie būtiną perkrovimą.
        }

        saveConfigurationToFile(); // Išsaugome atnaujintą konfigūraciją
        request->send(200, "application/json", "{\"success\":\"Configuration updated\"}");
        
        // Jei buvo pakeisti WiFi nustatymai, galbūt reikėtų iškart perkrauti ESP?
        // Pvz.: if (wifiChanged) { ESP.restart(); }
        // Bet tai nutrauktų ryšį staiga. Geriau leisti vartotojui tai padaryti.
      }
    }
  );

  // POST /config/time endpoint'as RTC laiko nustatymui
  server.on("/config/time", HTTP_POST, [](AsyncWebServerRequest *request){},
    NULL, // Nėra failo įkėlimo handler'io
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String postBody = "";
      if(total == 0) {
        request->send(400, "application/json", "{\"error\":\"Empty request body\"}");
        return;
      }
      for(size_t i=0; i<len; i++){
        postBody += (char)data[i];
      }

      if(index + len == total){ // Gautas visas body
        Serial.println("Received POST /config/time body: " + postBody);
        StaticJsonDocument<128> doc; // Pakankamai mažas JSON, tik laikas
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
        currentConfig.time = timeStr; // Atnaujiname konfigūracijos lauką
        saveConfigurationToFile(); // Išsaugome pakeitimus

        Serial.println("RTC time set to: " + timeStr);
        request->send(200, "application/json", "{\"success\":\"Time updated\", \"newTime\":\"" + timeStr + "\"}");
      }
    }
  );

  // GET /start endpoint'as - rankinis laistymo paleidimas
  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState == STATE_WATERING) {
      request->send(409, "application/json", "{\"error\":\"Watering already in progress\"}");
      return;
    }
    // Patikriname sąlygas prieš paleidžiant
    if (!checkWateringConditions()) {
      request->send(409, "application/json", "{\"error\":\"Conditions not met for watering\"}");
      return;
    }

    setState(STATE_WATERING);
    remainingWateringTimeSec = currentConfig.wateringDurationMin * 60;
    Serial.println("Manual watering started. Duration: " + String(currentConfig.wateringDurationMin) + " min.");
    
    StaticJsonDocument<128> doc;
    doc["status"] = "Watering started";
    doc["durationMin"] = currentConfig.wateringDurationMin;
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  // GET /stop endpoint'as - rankinis laistymo sustabdymas
  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request){
    if (currentState != STATE_WATERING) {
      request->send(409, "application/json", "{\"error\":\"No watering cycle in progress to stop\"}");
      return;
    }

    setState(STATE_IDLE); // Arba kita tinkama būsena, pvz., "WindowOpen", jei tai labiau tinka po rankinio sustabdymo
    remainingWateringTimeSec = 0;
    Serial.println("Manual watering stopped.");

    StaticJsonDocument<64> doc;
    doc["status"] = "Watering stopped";
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  // Aptarnauti statinius UI failus iš LittleFS /ui/ aplanko
  // Pavyzdžiui, http://192.168.4.1/ui/index.html
  server.serveStatic("/ui", LittleFS, "/ui/").setDefaultFile("index.html");

  server.begin(); // Paleisti web serverį
  Serial.println("HTTP serveris paleistas.");
}

// --- loop() funkcija ---
void loop() {
  // Nuskaityti dabartinį RTC laiką (pvz., kas sekundę)
  static unsigned long lastRtcReadTime = 0;
  if (millis() - lastRtcReadTime >= 1000) { // Kas 1000 ms = 1 sekundė
    lastRtcReadTime = millis();
    if (rtc.now().isValid()) { // Patikriname, ar laikas validus
        currentDateTime = rtc.now();
    } else {
        Serial.println("RTC laikas nevalidus!");
        // Galima bandyti iš naujo nustatyti laiką arba naudoti numatytąjį
        // Kol kas paliekame seną currentDateTime reikšmę
    }
    // Serial.print("Current RTC Time: "); Serial.println(currentDateTime.timestamp(DateTime::TIMESTAMP_ISO8601));
  }

  // Periodiškai nuskaityti jutiklius
  if (millis() - lastSensorReadTime >= currentConfig.sensorReadIntervalMs) {
    lastSensorReadTime = millis();

    // Nuskaityti BME280 duomenis
    if (bmeSuccessfullyInitialized) {
      currentTemperature = bme.readTemperature();
      currentHumidity = bme.readHumidity();
      currentPressure = bme.readPressure() / 100.0F; // hPa

      // Serial.print("Temp: "); Serial.print(currentTemperature); Serial.print(" *C, ");
      // Serial.print("Hum: "); Serial.print(currentHumidity); Serial.print(" %, ");
      // Serial.print("Pres: "); Serial.print(currentPressure); Serial.println(" hPa");
    } else {
      // Jei BME280 neveikia, nustatome į klaidos reikšmes
      currentTemperature = -999.0;
      currentHumidity = -999.0;
      currentPressure = -999.0;
    }

    // Nuskaityti vandens lygio jutiklį su debouncing
    // Įrašome naują reikšmę į masyvą
    // Efektyvus imčių skaičius pagal helper'į (apsauga nuo neteisingų reikšmių)
    int effectiveSamples = getEffectiveDebounceSamples();
    waterLevelReadings[waterLevelReadingIndex] = digitalRead(WATER_LEVEL_PIN);
    waterLevelReadingIndex = (waterLevelReadingIndex + 1) % effectiveSamples;

    // Tikriname, ar visos reikšmės masyve vienodos (stabilus signalas)
    bool allSame = true;
    for (int i = 0; i < effectiveSamples -1; i++) {
      if (waterLevelReadings[i] != waterLevelReadings[i+1]) {
        allSame = false;
        break;
      }
    }

    if (allSame) {
      if (stableWaterLevelState != waterLevelReadings[0]) { // Būsena pasikeitė
        stableWaterLevelState = waterLevelReadings[0];
        // Atnaujiname currentWaterLevelState pagal tai, ar pasiektas minState
        // Tarkime, jei minState yra "HIGH", tai HIGH yra minimalus lygis, o LOW yra "aukščiau minimalaus"
        // Jei minState yra "LOW", tai LOW yra minimalus lygis, o HIGH yra "aukščiau minimalaus"
        bool isMinLevel = (currentConfig.waterLevel.minState == "HIGH" && stableWaterLevelState == HIGH) || 
                          (currentConfig.waterLevel.minState == "LOW" && stableWaterLevelState == LOW);
        currentWaterLevelState = isMinLevel ? "MIN" : "OK"; // Arba "MAX", "ABOVE_MIN" ir pan.
        Serial.print("Water level changed to: "); Serial.println(currentWaterLevelState);
      }
    } 
    // Jei ne allSame, būsena nestabili, currentWaterLevelState nekeičiamas, lieka paskutinė stabili

    // TODO: Ateityje čia bus laistymo logika pagal RTC ir jutiklius
  }

  // Būsenų automato (state machine) logika
  switch (currentState) {
    case STATE_IDLE: {
      // Išparsuoti valandas, minutes, sekundes iš currentConfig.time
      // Formatas: YYYY-MM-DDTHH:MM:SS
      int targetHour = 0, targetMinute = 0, targetSecond = 0;
      if (currentConfig.time.length() == 19) { // Tikriname ilgį
        targetHour = currentConfig.time.substring(11, 13).toInt();
        targetMinute = currentConfig.time.substring(14, 16).toInt();
        targetSecond = currentConfig.time.substring(17, 19).toInt();
      }

      if (currentDateTime.isValid()) {
        int todayYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
        bool timeReached = (currentDateTime.hour() > targetHour) ||
                           (currentDateTime.hour() == targetHour && currentDateTime.minute() > targetMinute) ||
                           (currentDateTime.hour() == targetHour && currentDateTime.minute() == targetMinute && currentDateTime.second() >= targetSecond);
        if (timeReached && lastWateringYMD != todayYMD) {

          setState(STATE_WINDOW_OPEN);
          windowEndsAt = currentDateTime + TimeSpan(0, 0, currentConfig.toleranceWindowMin, 0); // Dienos, valandos, minutės, sekundės
          Serial.print("State changed to: WindowOpen. Window ends at: ");
          {
            char isoTime[20];
            sprintf(isoTime, "%04d-%02d-%02dT%02d:%02d:%02d",
                    windowEndsAt.year(), windowEndsAt.month(), windowEndsAt.day(),
                    windowEndsAt.hour(), windowEndsAt.minute(), windowEndsAt.second());
            Serial.println(isoTime);
          }
        }
      }
      break;
    }
    case STATE_WINDOW_OPEN: {
      // Tikrinti, ar langas dar galioja
      if (currentDateTime.isValid()) {
        if (currentDateTime >= windowEndsAt) {
          Serial.println("Watering window closed. No watering initiated or finished.");
          setState(STATE_IDLE);
          break;
        }
      }
      
      // Tikrinti jutiklius (vandens lygis, temperatūra).
      bool conditionsOk = checkWateringConditions();
      if (conditionsOk) {
        setState(STATE_WATERING);
        remainingWateringTimeSec = currentConfig.wateringDurationMin * 60;
        Serial.println("Conditions OK. State changed to: Watering");
      } else {
        // Sąlygos netinkamos, liekame WindowOpen ir laukiame, gal pagerės, kol langas atviras
        // Arba pereiti į ErrorPaused, jei pvz., vandens lygis per žemas ir tai yra klaida
      }
      break;
    }
    case STATE_WATERING: {
      // Laistymas valdomas per /start ir /stop komandas ir remainingWateringTimeSec
      // Čia reikės logikos automatiškai mažinti laiką, jei praleidome /start komandą
      // ir valdymas vyksta pilnai automatiškai.
      static unsigned long lastWateringSecondTick = 0;

      // Tikriname, ar reikia mažinti laiką (kas sekundę)
      if (millis() - lastWateringSecondTick >= 1000) {
        lastWateringSecondTick = millis();
        if (remainingWateringTimeSec > 0) {
          remainingWateringTimeSec--;
          // Serial.print("Watering, time left: "); Serial.println(remainingWateringTimeSec);
          // Tikriname sąlygas laistymo metu; jei blogos, pauzė su klaida
          if (!checkWateringConditions()) {
            Serial.println("Conditions became invalid during watering. Pausing with error.");
            setState(STATE_ERROR_PAUSED);
          }
        }       
      }

      if (remainingWateringTimeSec == 0) {
        // Laistymas baigtas
        Serial.println("Watering finished automatically or by timer.");
        setState(STATE_IDLE); // Arba WindowOpen, jei langas dar nesibaigė?
        // Pažymime, kad šiandien jau buvo laistyta
        if (currentDateTime.isValid()) {
          lastWateringYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
        }
      }
      break;
    }
    case STATE_ERROR_PAUSED: {
      // Periodiškai tikriname, ar sąlygos atsigavo
      static unsigned long lastErrorCheck = 0;
      if (millis() - lastErrorCheck >= (unsigned long)currentConfig.pauseResumeCheckIntervalMs) {
        lastErrorCheck = millis();
        if (checkWateringConditions()) {
          Serial.println("Error conditions cleared. Returning to Idle.");
          setState(STATE_IDLE);
        }
      }
      break;
    }
    default:
      // Nenumatyta būsena, grįžti į Idle
      Serial.println("Unknown system state! Returning to Idle.");
      setState(STATE_IDLE);
      break;
  }

  // Kitos asinchroninės užduotys, pvz., serverio užklausų apdorojimas, vyksta fone
}

// (enum helperiai perkelti į failo pradžią)

// --- Pagalbinės funkcijos ---

// Funkcija patikrinti, ar sąlygos tinkamos laistymui
bool checkWateringConditions() {
  // 1. Patikrinti vandens lygį
  // currentWaterLevelState nustatomas į "MIN", jei pasiektas minimalus lygis pagal konfigūraciją.
  if (currentWaterLevelState == "MIN") {
    Serial.println("Condition check: FAILED - Water level is at minimum.");
    return false;
  }

  // 2. Patikrinti BME280 duomenis (jei jutiklis veikia)
  if (bmeSuccessfullyInitialized) {
    if (currentTemperature < currentConfig.bme280.tempMin || currentTemperature > currentConfig.bme280.tempMax) {
      Serial.print("Condition check: FAILED - Temperature out of bounds (");
      Serial.print(currentTemperature); Serial.print(" *C, bounds: ");
      Serial.print(currentConfig.bme280.tempMin); Serial.print("-"); Serial.print(currentConfig.bme280.tempMax); Serial.println(" *C)");
      return false;
    }
    if (currentHumidity < currentConfig.bme280.humMin || currentHumidity > currentConfig.bme280.humMax) {
      Serial.print("Condition check: FAILED - Humidity out of bounds (");
      Serial.print(currentHumidity); Serial.print(" %, bounds: ");
      Serial.print(currentConfig.bme280.humMin); Serial.print("-"); Serial.print(currentConfig.bme280.humMax); Serial.println(" %)");
      return false;
    }
    if (currentPressure < currentConfig.bme280.presMin || currentPressure > currentConfig.bme280.presMax) {
      Serial.print("Condition check: FAILED - Pressure out of bounds (");
      Serial.print(currentPressure); Serial.print(" hPa, bounds: ");
      Serial.print(currentConfig.bme280.presMin); Serial.print("-"); Serial.print(currentConfig.bme280.presMax); Serial.println(" hPa)");
      return false;
    }
  } else {
    // Jei BME280 neveikia, o konfigūracijoje yra nustatytos ribos, laikome, kad sąlygos netinkamos.
    // Ateityje galima pridėti konfigūracijos parinktį ignoruoti BME klaidas.
    // Tikriname, ar bent viena BME riba yra nustatyta (ne numatytoji 0.0, kas gali reikšti 'nesvarbu')
    // Šis patikrinimas yra grubus, geriau būtų turėti aiškią vėliavėlę 'enforceBmeLimits'
    bool bmeLimitsSet = (currentConfig.bme280.tempMin != 0.0 || currentConfig.bme280.tempMax != 0.0 ||
                         currentConfig.bme280.humMin != 0.0 || currentConfig.bme280.humMax != 0.0 ||
                         currentConfig.bme280.presMin != 0.0 || currentConfig.bme280.presMax != 0.0);
    if (bmeLimitsSet) {
        Serial.println("Condition check: FAILED - BME280 sensor not initialized, but limits are set in config.");
        return false;
    }
    // Jei BME neveikia ir ribos nenustatytos (arba nuspręsta ignoruoti), tęsiame tik su vandens lygiu
    Serial.println("Condition check: SKIPPED BME280 - Sensor not initialized and/or no limits set.");
  }

  Serial.println("Condition check: PASSED - All conditions are OK for watering.");
  return true;
}
