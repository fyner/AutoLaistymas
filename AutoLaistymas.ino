// Forward declarations to satisfy Arduino's auto-generated prototypes
struct Config;
enum SystemStateEnum : uint8_t;

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "RTClib.h" // RTC biblioteka
#include <Adafruit_BME280.h> // BME280 biblioteka
#include <Adafruit_Sensor.h> // Būtina Adafruit jutiklių bibliotekoms
#include <Wire.h> // I2C reikalingas RTC/BME

// !!! SVARBU: Pakeiskite WATER_LEVEL_PIN į kontaktą, kurį naudojate vandens lygio jutikliui !!!
#define WATER_LEVEL_PIN 13 // Pavyzdys, pakeiskite pagal savo schemą
// !!! SVARBU: Pakeiskite RELAY_PIN į kontaktą, kuriuo valdote relę (siurblį/vožtuvą) !!!
#define RELAY_PIN 12 // Pavyzdys, pakeiskite pagal savo schemą

// Maksimalus debouncing imčių kiekis
#define MAX_DEBOUNCE_SAMPLES 10
// Maksimalus suplanuotų laistymo laikų skaičius per dieną
#define MAX_WATERING_SLOTS 8

// Forward deklaracija, kad būtų galima naudoti setup() anksčiau
bool parseIsoDateTime(const String &iso, DateTime &out);

// --- Būsenų enum ir konvertavimo funkcijos (perkeltos aukščiau, kad būtų prieinamos visur) ---
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

// ISO 8601 (YYYY-MM-DDTHH:MM:SS) parseris į RTClib DateTime
bool parseIsoDateTime(const String &iso, DateTime &out) {
  if (iso.length() < 19) return false;
  // Tikimės formato: YYYY-MM-DDTHH:MM:SS
  int Y = iso.substring(0, 4).toInt();
  int M = iso.substring(5, 7).toInt();
  int D = iso.substring(8, 10).toInt();
  int h = iso.substring(11, 13).toInt();
  int m = iso.substring(14, 16).toInt();
  int s = iso.substring(17, 19).toInt();
  // Minimalus formatinis patikrinimas
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
  // Keli dienos laikai HH:MM formatu
  int wateringTimesCount;
  String wateringTimes[MAX_WATERING_SLOTS];
  WaterLevelConfig waterLevel;
  BME280Config bme280;
  WifiConfig wifi;
  RelayConfig relay;
  String apiToken; // API prieigos tokenas; jei tuščias – autentifikacija nevykdoma
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
static inline void configToJson(JsonDocument &doc, const Config &cfg, bool includeSensitive = false) {
  doc["time"] = cfg.time;
  doc["wateringDurationMin"] = cfg.wateringDurationMin;
  doc["toleranceWindowMin"] = cfg.toleranceWindowMin;
  doc["sensorReadIntervalMs"] = cfg.sensorReadIntervalMs;
  doc["pauseResumeCheckIntervalMs"] = cfg.pauseResumeCheckIntervalMs;
  // wateringTimes masyvas
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
    // Įrašyti jautrius laukus tik į failą, bet ne į GET /config atsakymą
    doc["apiToken"] = cfg.apiToken;
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
      // Minimalus formatas HH:MM
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

  if (doc.containsKey("apiToken")) {
    cfg.apiToken = doc["apiToken"].as<String>();
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
// Daugybiniams laikams: kiekvienam slot'ui atskiras žymeklis, kad nevykdyti daugiau nei kartą per dieną
int lastWateringYMDForSlot[MAX_WATERING_SLOTS]; // YYYYMMDD
int activeSlotIndex = -1; // kuris slot'as šiuo metu aktyvus WINDOW_OPEN/WATERING metu

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
  // Numatyti du laistymo laikai per dieną
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
  // Relė pagal nutylėjimą aktyvi LOW (dauguma modulių); pinas – #define RELAY_PIN
  currentConfig.relay.activeLevel = "LOW";
  currentConfig.apiToken = ""; // pagal nutylėjimą autentifikacija išjungta
  
  Serial.println("Loaded default configuration.");
}

// Konfigūracijos validacija ir ribojimai
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
  // Rašyti į laikiną failą
  File tmp = LittleFS.open(tmpPath, "w");
  if (!tmp) {
    Serial.println("Failed to create temp config file for writing.");
    return;
  }
  StaticJsonDocument<2048> doc;
  configToJson(doc, currentConfig, true /* includeSensitive */);
  if (serializeJson(doc, tmp) == 0) {
    Serial.println(F("Failed to write JSON to temp config"));
    tmp.close();
    LittleFS.remove(tmpPath);
    return;
  }
  tmp.flush();
  tmp.close();
  // Pervadinti atominiu būdu (kiek leidžia FS)
  if (LittleFS.exists(dstPath)) LittleFS.remove(dstPath);
  if (!LittleFS.rename(tmpPath, dstPath)) {
    Serial.println("Failed to rename temp config to /config.json");
  } else {
    Serial.println(F("Configuration successfully saved to /config.json."));
  }
}

// --- Globalūs objektai ---
AsyncWebServer server(80); // Web serveris ant 80 porto
RTC_DS3231 rtc;           // RTC objektas (DS3231)
Adafruit_BME280 bme;      // BME280 objektas (I2C)

// Kintamieji jutiklių nuskaitymui ir debouncing
unsigned long lastSensorReadTime = 0;
unsigned long lastWaterLevelSampleTime = 0; // atskiras vandens jutiklio mėginių intervalas
int waterLevelReadings[10]; // Masyvas debouncing'ui, dydis pagal max debounceSamples
int waterLevelReadingIndex = 0;
int stableWaterLevelState = -1; // -1 reiškia neapsispręsta, 0 LOW, 1 HIGH
bool bmeSuccessfullyInitialized = false;

// --- setup() funkcija ---
void setup() {
  Serial.begin(115200);
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
  // ESP32: prieš rtc.begin() būtina paleisti I2C
  Wire.begin();
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
        // Iškart atnaujinti currentDateTime, kad /status rodytų naują laiką
        currentDateTime = cfgTime;
      }
    }
    Serial.println("RTC modulis rastas.");
    // Pradinė sinchronizacija su RTC
    DateTime now = rtc.now();
    if (now.isValid()) {
      currentDateTime = now;
    }
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
  // Užpildyti pradinius debouncing masyvo įrašus (apribojus imčių skaičių)
  validateAndClampConfig(currentConfig);
  int initSamples = getEffectiveDebounceSamples();
  for (int i = 0; i < initSamples; i++) {
    waterLevelReadings[i] = digitalRead(WATER_LEVEL_PIN);
  }
  waterLevelReadingIndex = 0;
  stableWaterLevelState = waterLevelReadings[0];
  // "NERA" – kai pasiektas minimalus lygis (vandens nėra); "YRA" – kai vanduo pakankamas
  currentWaterLevelState = (stableWaterLevelState == (currentConfig.waterLevel.minState == "HIGH" ? HIGH : LOW)) ? "NERA" : "YRA";
  Serial.print("Water level changed to: "); Serial.println(currentWaterLevelState);

  // Apsaugos jau pritaikytos per validateAndClampConfig

  // Inicializuoti relę
  // Jei config.relay.activeLevel neapibrėžtas, laikome LOW kaip saugią numatytąją
  if (!(currentConfig.relay.activeLevel == "LOW" || currentConfig.relay.activeLevel == "HIGH")) {
    currentConfig.relay.activeLevel = "LOW";
  }
  relayActiveLow = (currentConfig.relay.activeLevel == "LOW");
  pinMode(RELAY_PIN, OUTPUT);
  turnRelayOff(); // Saugiai išjungta paleidimo metu
  relayIsOn = false;

  // 6. Web serverio maršrutai (su autentifikacija POST metodams)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    // Rodyti tik UI failą iš LittleFS; jokių inline HTML
    if (LittleFS.exists("/ui/index.html")) {
      request->send(LittleFS, "/ui/index.html", "text/html");
    } else {
      request->send(404, "text/plain", "UI file /ui/index.html not found in LittleFS");
    }
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

    // Papildomai: iki kito suplanuoto starto (sekundėmis)
    long nextStartInSec = -1;
    if (currentDateTime.isValid() && currentConfig.wateringTimesCount > 0) {
      // Raskime artimiausią šiandien >= dabar, kitu atveju rytojaus pirmą
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
        // Rytojus, imame mažiausią laiką
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
        if (nextStartInSec < 0) nextStartInSec = 0; // apsauga
      }
    }
    doc["nextStartInSec"] = nextStartInSec; // -1 jei nerasta

    // Likęs lango laikas (sekundėmis), jei langas atidarytas
    long windowRemainingSec = 0;
    if (currentDateTime.isValid() && currentState == STATE_WINDOW_OPEN) {
      if (currentDateTime < windowEndsAt) {
        TimeSpan rem = windowEndsAt - currentDateTime;
        windowRemainingSec = (long)rem.totalseconds();
      }
    }
    doc["windowRemainingSec"] = windowRemainingSec;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  // GET /config endpoint'as
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<2048> doc; // Dydis turi atitikti save/load funkcijų dydį
    configToJson(doc, currentConfig, false);
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
  });

  // POST /config endpoint'as konfigūracijos atnaujinimui
  server.on("/config", HTTP_POST,
    [](AsyncWebServerRequest *request){ /* response bus siunčiamas body handler'yje */ },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0) {
        request->_tempObject = new String();
      }
      String *body = reinterpret_cast<String*>(request->_tempObject);
      body->reserve(total);
      body->concat((const char*)data, len);

      if (index + len == total) {
        // Autentifikacija (jei nustatytas tokenas)
        if (currentConfig.apiToken.length() > 0) {
          AsyncWebHeader* h = request->getHeader("X-API-Token");
          if (!h || h->value() != currentConfig.apiToken) {
            delete body; request->_tempObject = nullptr;
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
          }
        }
        String postBody = *body;
        delete body;
        request->_tempObject = nullptr;

        Serial.println("Received POST /config body: " + postBody);
        StaticJsonDocument<2048> doc; // Dydis turi atitikti load/save funkcijų dydį
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

        // Jei pasikeitė debounce parametrai – reinicializuoti mėginių buferį
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

  // POST /config/time endpoint'as RTC laiko nustatymui
  server.on("/config/time", HTTP_POST,
    [](AsyncWebServerRequest *request){ /* response bus siunčiamas body handler'yje */ },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0) {
        request->_tempObject = new String();
      }
      String *body = reinterpret_cast<String*>(request->_tempObject);
      body->reserve(total);
      body->concat((const char*)data, len);

      if (index + len == total) {
        if (currentConfig.apiToken.length() > 0) {
          AsyncWebHeader* h = request->getHeader("X-API-Token");
          if (!h || h->value() != currentConfig.apiToken) {
            delete body; request->_tempObject = nullptr;
            request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
            return;
          }
        }
        String postBody = *body;
        delete body;
        request->_tempObject = nullptr;

        Serial.println("Received POST /config/time body: " + postBody);
        StaticJsonDocument<256> doc; // Pakanka laukui 'time'
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

  // POST /start endpoint'as - rankinis laistymo paleidimas
  server.on("/start", HTTP_POST, [](AsyncWebServerRequest *request){
    if (currentConfig.apiToken.length() > 0) {
      AsyncWebHeader* h = request->getHeader("X-API-Token");
      if (!h || h->value() != currentConfig.apiToken) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    }
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

  // POST /stop endpoint'as - rankinis laistymo sustabdymas
  server.on("/stop", HTTP_POST, [](AsyncWebServerRequest *request){
    if (currentConfig.apiToken.length() > 0) {
      AsyncWebHeader* h = request->getHeader("X-API-Token");
      if (!h || h->value() != currentConfig.apiToken) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    }
    if (currentState != STATE_WATERING) {
      request->send(409, "application/json", "{\"error\":\"No watering cycle in progress to stop\"}");
      return;
    }
  // POST /restart endpoint'as - prietaiso perkrovimas
  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    if (currentConfig.apiToken.length() > 0) {
      AsyncWebHeader* h = request->getHeader("X-API-Token");
      if (!h || h->value() != currentConfig.apiToken) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    }
    request->send(200, "application/json", "{\"success\":\"Restarting\"}");
    delay(100);
    ESP.restart();
  });

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

  // Periodiškai nuskaityti aplinkos jutiklius (BME)
  if (millis() - lastSensorReadTime >= (unsigned long)currentConfig.sensorReadIntervalMs) {
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
  }

  // Vandens lygio jutiklio mėginių ėmimas pagal debounceIntervalMs
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

  // Būsenų automato (state machine) logika
  switch (currentState) {
    case STATE_IDLE: {
      // Patikrinti, ar atėjo kuris nors suplanuotas laikas HH:MM
      if (currentDateTime.isValid()) {
        int todayYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
        // Inicializuoti per pirmą kartą (jeigu dar ne)
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
          // Suformuoti šiandienos suplanuotą laiką HH:MM:00
          DateTime scheduled(currentDateTime.year(), currentDateTime.month(), currentDateTime.day(), targetHour, targetMinute, 0);
          TimeSpan tol(0, 0, currentConfig.toleranceWindowMin, 0);
          bool inWindow = (currentDateTime >= scheduled) && (currentDateTime < (scheduled + tol));
      if (inWindow && lastWateringYMDForSlot[i] != todayYMD) {
            setState(STATE_WINDOW_OPEN);
            activeSlotIndex = i;
            windowEndsAt = scheduled + tol; // lango pabaiga pagal suplanuotą laiką
            Serial.print("State changed to: WindowOpen (slot "); Serial.print(i); Serial.println(")");
            Serial.print("Window ends at: ");
            char isoTime[20];
            sprintf(isoTime, "%04d-%02d-%02dT%02d:%02d:%02d",
                    windowEndsAt.year(), windowEndsAt.month(), windowEndsAt.day(),
                    windowEndsAt.hour(), windowEndsAt.minute(), windowEndsAt.second());
            Serial.println(isoTime);
            break; // vienu metu tik vienas langas
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
          activeSlotIndex = -1;
          break;
        }
      }
      
      // Tikrinti jutiklius (vandens lygis, temperatūra).
      bool conditionsOk = checkWateringConditions();
      if (conditionsOk) {
        setState(STATE_WATERING);
        remainingWateringTimeSec = currentConfig.wateringDurationMin * 60;
        Serial.println("Conditions OK. State changed to: Watering");
        // Pažymėti, kad šiandien šiam slot'ui laistymas pradėtas
        if (currentDateTime.isValid()) {
          int todayYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
          if (activeSlotIndex >= 0 && activeSlotIndex < MAX_WATERING_SLOTS) {
            lastWateringYMDForSlot[activeSlotIndex] = todayYMD;
          }
        }
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
          int todayYMD = currentDateTime.year()*10000 + currentDateTime.month()*100 + currentDateTime.day();
          if (activeSlotIndex >= 0 && activeSlotIndex < MAX_WATERING_SLOTS) {
            lastWateringYMDForSlot[activeSlotIndex] = todayYMD;
          }
        }
        // Nebelieka aktyvaus lango/slot'o po užbaigimo
        activeSlotIndex = -1;
      }
      break;
    }
    case STATE_ERROR_PAUSED: {
      // Periodiškai tikriname, ar sąlygos atsigavo
      static unsigned long lastErrorCheck = 0;
      if (millis() - lastErrorCheck >= (unsigned long)currentConfig.pauseResumeCheckIntervalMs) {
        lastErrorCheck = millis();
        if (checkWateringConditions()) {
          // Jei langas dar atviras – tęsiame; kitaip grįžtame į Idle
          if (currentDateTime.isValid() && currentDateTime < windowEndsAt) {
            Serial.println("Error conditions cleared. Resuming watering.");
            setState(STATE_WATERING);
          } else {
            Serial.println("Error conditions cleared but window closed. Returning to Idle.");
            setState(STATE_IDLE);
            activeSlotIndex = -1;
          }
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
  // Naudojame statuso kodą ir loguojame tik kai pasikeičia
  // 0=unknown, 1=OK, 2=FAIL_MIN_LEVEL, 3=FAIL_TEMP, 4=FAIL_HUM, 5=FAIL_PRES, 6=FAIL_BME_NOT_INIT_LIMITS, 7=SKIP_BME
  static int lastStatus = 0;
  int status = 0;
  bool ok = true;

  // 1. Vandens lygis
  if (currentWaterLevelState == "NERA") {
    status = 2; ok = false; // minimumas pasiektas
  }

  // 2. BME280 (jei jutiklis veikia ir kol kas OK)
  if (ok && bmeSuccessfullyInitialized) {
    if (currentTemperature < currentConfig.bme280.tempMin || currentTemperature > currentConfig.bme280.tempMax) {
      status = 3; ok = false;
    } else if (currentHumidity < currentConfig.bme280.humMin || currentHumidity > currentConfig.bme280.humMax) {
      status = 4; ok = false;
    } else if (currentPressure < currentConfig.bme280.presMin || currentPressure > currentConfig.bme280.presMax) {
      status = 5; ok = false;
    } else {
      status = 1; // OK pagal BME ir vandens lygį
    }
  } else if (ok && !bmeSuccessfullyInitialized) {
    bool bmeLimitsSet = (currentConfig.bme280.tempMin != 0.0 || currentConfig.bme280.tempMax != 0.0 ||
                         currentConfig.bme280.humMin != 0.0 || currentConfig.bme280.humMax != 0.0 ||
                         currentConfig.bme280.presMin != 0.0 || currentConfig.bme280.presMax != 0.0);
    if (bmeLimitsSet) { status = 6; ok = false; } else { status = 7; ok = true; }
  }

  // Loguoti tik pasikeitus būsenai
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
