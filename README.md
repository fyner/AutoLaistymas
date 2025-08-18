# 🌱 AutoLaistymas

Automatinis laistymo sistema, pagrįsta ESP32 mikrovaldikliu.

## 📱 Vartotojo sąsaja

- **Pagrindinis puslapis** (`/ui/index.html`) - sistemos būsena ir valdymas
- **Nustatymai** (`/ui/settings.html`) - konfigūracija
- **Responsive dizainas** - optimizuota mobiliems įrenginiams
- **Tamsus/šviesus stilius** - automatinis perjungimas

## 🔌 API

REST API be autentifikacijos:

- `GET /status` - sistemos būsena
- `POST /start` - pradėti laistymą
- `POST /stop` - sustabdyti laistymą
- `GET /config` - gauti konfigūraciją
- `POST /config` - atnaujinti konfigūraciją

## ⚙️ Konfigūracija

Visi nustatymai išsaugo automatiškai į `config.json` failą:

- **Laistymo laikai** - kada paleisti laistymą
- **Vandens lygio jutiklis** - debouncing ir logika
- **BME280 sensorius** - temperatūros, drėgmės, slėgio ribos
- **WiFi SoftAP** - tinklo pavadinimas ir slaptažodis

## 🚀 Naudojimas

1. **Prijunkite ESP32** prie maitinimo
2. **Prisijunkite** prie `AutoLaistymas` WiFi
3. **Atidarykite** `http://192.168.4.1`
4. **🎮 Valdykite** sistemą per pagrindinį puslapį
5. **⚙️ Nustatykite** parametrus nustatymų puslapyje

## 📋 Reikalavimai

### Hardware:
- ESP32 mikrovaldiklis
- WiFi prijungimas
- Vandens lygio jutiklis (opcionaliai)
- BME280 sensorius (opcionaliai)
- RTC modulis (opcionaliai)

### Software:
- Arduino IDE arba PlatformIO
- ESP32 board support
- Reikalingos bibliotekos (WiFi, AsyncWebServer, LittleFS)