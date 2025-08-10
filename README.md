# AutoLaistymas - ESP32 Automatinė Laistymo Sistema

## Apžvalga
Šis projektas skirtas sukurti automatinę laistymo sistemą, naudojant ESP32 mikrovaldiklį. Sistema veikia kaip Wi-Fi prieigos taškas (SoftAP), leidžiantis konfigūraciją ir valdymą per vietinį tinklą be išorinės interneto prieigos. Konfigūracija saugoma `config.json` faile LittleFS failų sistemoje. Sistema integruoja BME280 jutiklį (temperatūra, drėgmė, slėgis), vandens lygio jutiklį ir RTC modulį laikui sekti. Valdymas ir būsenos stebėjimas galimas per REST API ir planuojamą vartotojo sąsają.

## Pagrindinės Savybės
- **SoftAP Režimas:** ESP32 veikia kaip Wi-Fi prieigos taškas (numatytasis SSID: `AutoLaistymas`, slaptažodis: `esp32automatinis`).
- **Konfigūracija per Failą:** Visi nustatymai saugomi `config.json` faile LittleFS.
- **Jutiklių Integracija:**
    - BME280: temperatūros, drėgmės, atmosferos slėgio matavimas.
    - Vandens Lygio Jutiklis: vandens talpos lygio stebėjimas.
- **RTC Laikrodis:** Tikslus laiko sekimas naudojant RTC modulį (pvz., DS3231).
- **REST API:** Leidžia gauti sistemos būseną, konfigūraciją ir ją keisti.
- **Vartotojo Sąsaja (UI):** Planuojama statinė vartotojo sąsaja, pasiekiama per `/ui/` maršrutą.
- **Būsenų Automatas (State Machine):** Sistema veikia pagal apibrėžtą būsenų seką (Idle, WindowOpen, Watering, ErrorPaused) (planuojama).

## Reikalinga Aparatūra
- ESP32 vystymo plokštė
- BME280 jutiklis (I2C)
- Vandens lygio jutiklis (skaitmeninis arba analoginis)
- RTC modulis (pvz., DS3231, I2C)
- Relės modulis vandens siurblio ar vožtuvo valdymui
- Maitinimo šaltinis

## Programinė Įranga ir Bibliotekos
- **IDE:** Arduino IDE
- **Kalba:** C++ (Arduino Framework)
- **Pagrindinės Bibliotekos:**
    - `WiFi.h` (ESP32 Wi-Fi valdymui)
    - `ESPAsyncWebServer.h` ir `AsyncTCP.h` (Asinchroniniam web serveriui)
    - `LittleFS.h` (Failų sistemai)
    - `ArduinoJson.h` (JSON apdorojimui)
    - `RTClib.h` (RTC valdymui)
    - `Adafruit_BME280.h` ir `Adafruit_Sensor.h` (BME280 jutikliui)

## Projekto Failų Struktūra
```
AutoLaistymas/
├── AutoLaistymas.ino         # Pagrindinis Arduino programos failas
├── data/                     # Aplankas, kurio turinys įkeliamas į LittleFS
│   ├── config.json           # Sistemos konfigūracijos failas (sukurtas automatiškai)
│   └── ui/                   # Aplankas vartotojo sąsajos failams
│       └── index.html        # Pagrindinis UI HTML failas (pavyzdys)
│       └── (kiti css, js failai...)
├── README.md                 # Šis dokumentacijos failas
└── api_documentation.json    # Detali API endpoint'ų dokumentacija JSON formatu (bus sukurta)
```

## Įdiegimas ir Paleidimas
1.  **Arduino IDE Paruošimas:**
    *   Įdiekite [Arduino IDE](https://www.arduino.cc/en/software).
    *   Pridėkite ESP32 plokščių palaikymą Arduino IDE (instrukcijos [čia](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)).
    *   Per Arduino IDE Library Manager įdiekite visas aukščiau (`Programinė Įranga ir Bibliotekos` skiltyje) nurodytas bibliotekas.
2.  **ESP32 Failų Sistemos Įkėlimo Įrankis:**
    *   Įdiekite "ESP32 Sketch Data Upload" įrankį (arba "LittleFS ESP32 Uploader", priklausomai nuo versijos) į Arduino IDE. Instrukcijas rasite paieškoje (pvz., "ESP32 LittleFS upload tool Arduino IDE").
3.  **Aparatūros Pajungimas:** Prijunkite visus jutiklius ir modulius prie ESP32 pagal jų specifikacijas.
4.  **Kodo Įkėlimas:**
    *   Atidarykite `AutoLaistymas.ino` Arduino IDE.
    *   Pasirinkite teisingą ESP32 plokštę ir prievadą (port).
    *   Įkelkite programos kodą į ESP32.
5.  **Duomenų Įkėlimas į LittleFS:**
    *   Paruoškite `data/` aplanką su `ui/` poaplankiu ir jame esančiais UI failais (bent jau `index.html`). `config.json` bus sukurtas automatiškai pirmo paleidimo metu, jei jo nėra.
    *   Arduino IDE meniu pasirinkite `Tools > ESP32 Sketch Data Upload` (arba panašų pavadinimą), kad įkeltumėte `data/` aplanko turinį į ESP32 LittleFS.
6.  **Prisijungimas:**
    *   Po sėkmingo paleidimo ESP32 sukurs Wi-Fi prieigos tašką (AP).
    *   Prisijunkite prie Wi-Fi tinklo:
        *   **SSID:** `AutoLaistymas` (arba kaip nurodyta `config.json`)
        *   **Slaptažodis:** `esp32automatinis` (arba kaip nurodyta `config.json`)
    *   Naršyklėje atidarykite `http://192.168.4.1` (numatytasis ESP32 AP IP adresas).

## Konfigūracija (`config.json`)
Sistemos konfigūracija valdoma per `config.json` failą, esantį LittleFS. Jei failas nerandamas, jis sukuriamas su numatytosiomis reikšmėmis.
Failo struktūra ir parametrai detaliai aprašyti `api_documentation.json` faile (bus sukurtas) ir matomi per `GET /config` API užklausą. Pagrindinės konfigūruojamos sekcijos:
- `time`: Dabartinis laikas (ISO 8601 formatu).
- `wateringDurationMin`: Laistymo trukmė minutėmis.
- `toleranceWindowMin`: Laistymo pradžios tolerancijos langas minutėmis.
- `sensorReadIntervalMs`: Jutiklių nuskaitymo intervalas milisekundėmis.
- `pauseResumeCheckIntervalMs`: Pauzės/atnaujinimo tikrinimo intervalas milisekundėmis.
- `waterLevel`: Vandens lygio jutiklio nustatymai.
- `bme280`: BME280 jutiklio nustatymai (min/max ribos).
- `wifi`: SoftAP Wi-Fi nustatymai (SSID, slaptažodis, kanalas, paslėptis).

## API Endpoints
Detali API endpoint'ų dokumentacija su pavyzdžiais bus pateikta `api_documentation.json` faile.
Trumpas sąrašas:
- **`GET /`**: Grąžina pagrindinį HTML puslapį su nuorodomis.
- **`GET /status`**: Grąžina dabartinę sistemos būseną (jutiklių duomenys, laistymo būsena, kt.).
- **`GET /config`**: Grąžina visą dabartinę `config.json` konfigūraciją.
- **`POST /config`**: Atnaujina visą arba dalį konfigūracijos. Kūne turi būti JSON su norimais pakeitimais. Išsaugo pakeitimus į `config.json`.
- **`POST /config/time`**: Nustato RTC laiką. Kūne turi būti JSON objektas su `time` raktu, pvz., `{"time": "YYYY-MM-DDTHH:MM:SS"}`.
- **`GET /ui/*`**: Aptarnauja statinius failus iš `/ui/` aplanko LittleFS (pvz., `/ui/index.html`).

Planuojami ateityje:
- `GET /start`: Pradeda laistymo ciklą rankiniu būdu (jei sąlygos leidžia).
- `GET /stop`: Sustabdo aktyvų laistymo ciklą.
- Kiti būsenų automatui ir valdymui reikalingi endpoint'ai.