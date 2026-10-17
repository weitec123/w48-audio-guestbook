# ☎️ W48 Audio Guestbook (ESP32-S3)

Umbau eines historischen **W48 Bakelit-Tischtelefons** zu einem autarken Audio-Gästebuch mit Web-Interface, MicroSD-Speicher, Wählscheiben-Easter-Eggs und Erdtasten-Steuerung.

## ✨ Features
- **Unkomprimierte 16-Bit PCM Studio-Qualität (16 kHz WAV)** direkt auf MicroSD-Karte (Fallback auf internen LittleFS-Flash).
- **100 % Störungsfreier Ton:**
  - I2S-Takt (`MAX98357A`) wird im Ruhezustand komplett gestoppt.
  - WLAN schaltet sich beim Abnehmen des Hörers vollständig ab (`WIFI_OFF`) und verbindet sich nach dem Auflegen automatisch wieder.
  - Verlustfreier Sofort-Start der Aufnahme über einen 8192-Sample RAM-Ringpuffer, während die SD-Karte im Hintergrund die Datei anlegt.
- **Erdtaste (Taster auf GPIO 9):** Unterbricht jederzeit die Ansage/Aufnahme und spielt sofort die zuletzt aufgenommene Nachricht am Hörer ab.
- **Wählscheiben-Codes (Toleranz-Raster `1`, `5`, `9`):**
  - `1`: Letzte Aufnahme anhören
  - `5`: Easter Egg 1 (Witz) anhören
  - `9`: Easter Egg 2 (Ständchen) anhören
  - `199`: Neue Begrüßungsansage direkt am Hörer aufsprechen
  - `599`: Easter Egg 1 neu aufsprechen
  - `999`: Easter Egg 2 neu aufsprechen
  - `9999`: Zufalls-Klingeln (Glocke) an-/ausschalten
- **Web-Dashboard (`http://audio-guestbook.local`):**
  - Live-Speicherplatzanzeige (MB & Stunden/Minuten)
  - Lautstärke & Mikrofon-Verstärkung (Gain) stufenlos einstellbar
  - Einzel-Download, Direkt-Player im Browser und **1-Klick ZIP-Download** aller Aufnahmen
  - Eigene `.wav`-Dateien für Ansage und Easter Eggs hochladen

## 🔌 Pinbelegung (ESP32-S3)

| Komponente | Pin am Modul | ESP32-S3 GPIO |
| :--- | :--- | :--- |
| **MAX98357A (I2S Verstärker)** | BCLK | GPIO 4 |
| | LRC / WS | GPIO 5 |
| | DIN / DOUT | GPIO 6 |
| **MAX9814 (Mikrofon)** | OUT | GPIO 1 (ADC) |
| | VDD | 3.3V |
| **Gabelumschalter (Hörer)** | Schließer gegen GND | GPIO 7 (`INPUT_PULLUP`) |
| **Wählscheibe (nsi Impuls)** | Impulskontakt gegen GND | GPIO 8 (`INPUT_PULLUP`) |
| **Erdtaste (Knopf)** | Schließer gegen GND | GPIO 9 (`INPUT_PULLUP`) |
| **MicroSD-Kartenleser (3.3V)** | CS | GPIO 10 |
| | MOSI | GPIO 13 |
| | SCK / CLK | GPIO 12 |
| | MISO | GPIO 11 |

## 🛠️ Kompilieren & Flashen (Arduino CLI)

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=cdc audio_guestbook
arduino-cli upload -p /dev/cu.usbmodem2101 --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=cdc audio_guestbook
```
