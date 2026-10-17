#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include "sd_diskio.h"
extern "C" {
#include "diskio.h"
}
#include <driver/i2s.h>
#include <math.h>
#include <vector>
#include "greeting_wav.h"
#include "default_easter1_wav.h"
#include "default_easter7_wav.h"
#include "ring_toggle_wav.h"
#include "index_html.h"

// Hardware Pins (ESP32-S3)
#define I2S_BCLK 4
#define I2S_LRC  5
#define I2S_DOUT 6
#define HOOK_PIN 7

// Waehlscheibe (N38 Impulskontakt)
#define DIAL_PULSE_PIN 8

// Taster (Erdtaste / Schließer auf Pin 9 und GND)
#define BUTTON_PIN 9

// MicroSD-Kartenleser (SPI: MOSI=13, MISO=11 per Pin-Scanner ermittelt!)
#define SD_CS   10
#define SD_MOSI 13
#define SD_SCK  12
#define SD_MISO 11

bool sdCardMounted = false;
fs::FS* storageFS = &LittleFS;

// Analoges Mikrofon MAX9814
#define MIC_PIN 1

// Glocken-Ansteuerung (vorbereitet fuer H-Bruecke / Transistor)
#define BELL_PIN_A 15
#define BELL_PIN_B 16

#define SAMPLE_RATE 16000

WebServer server(80);
Preferences prefs;

// Konfiguration (im Flash)
int currentVolume = 50;
int micGain = 10; // Aufnahme-Verstaerkung (1 .. 25)
String wifiSSID = "";
String wifiPass = "";
String webhookURL = "";
int minRecordSec = 2;
int maxRecordSec = 180;
bool ringActive = false;
int ringIntervalMin = 30;

// Wählscheiben-Codes (frei konfigurierbar)
String codeLastRec = "0";
String codeRingToggle = "99";
String codeAdminPin = "123";
String codeEaster1 = "1";
String codeEaster2 = "7";
String codeRecEaster1 = "111";
String codeRecEaster2 = "777";

// Aufnahme-Modus: 0 = Gast, 1 = Neue Ansage, 2 = Neuer Witz (Easter 1), 3 = Neues Ständchen (Easter 2)
int recordMode = 0;

// Zustandsvariablen
bool wasOffHook = false;
bool isRecording = false;
File currentRecordFile;
String currentRecordFilename = "";
uint32_t recordDataBytes = 0;
int nextRecordNum = 1;
unsigned long recordStartMillis = 0;
unsigned long lastRingCheckMillis = 0;
int32_t micDcBias = 1450;
bool lastButtonState = HIGH;
unsigned long lastButtonDebounce = 0;
bool buttonPressDetected = false;

// 16 kHz Hardware-Timer & Ringpuffer fuer lueckenlose Aufnahme
#define RING_BUF_SIZE 8192
int16_t ringBuffer[RING_BUF_SIZE];
volatile uint32_t ringHead = 0;
uint32_t ringTail = 0;
hw_timer_t *recTimer = NULL;
int32_t lpFiltered = 0;
void IRAM_ATTR onRecordSample() {
  if (isRecording) {
    int raw = analogRead(MIC_PIN);
    micDcBias = (micDcBias * 127 + raw) / 128;
    int32_t centered = raw - micDcBias;
    // Kein dumpfer Tiefpassfilter mehr noetig, da WLAN waehrend der Aufnahme komplett AUS ist!
    // Volle 16 kHz Sprachbrillanz (Hoehen komplett erhalten):
    int32_t val = centered * micGain;
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;

    uint32_t nextHead = (ringHead + 1) % RING_BUF_SIZE;
    if (nextHead != ringTail) {
      ringBuffer[ringHead] = (int16_t)val;
      ringHead = nextHead;
    }
  }
}

// Waehlscheiben-Erkennung
volatile int pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
volatile uint32_t pulseDeltas[20];
String dialedCode = "";
unsigned long lastDialActivity = 0;

void IRAM_ATTR onDialPulse() {
  unsigned long now = millis();
  unsigned long delta = now - lastPulseTime;
  // W48 Impulskontakt taktet mit ca. 100ms Periode.
  // 60ms Entprellung filtert jedes Prellen sauber heraus.
  if (delta > 60) {
    if (pulseCount < 20) {
      pulseDeltas[pulseCount] = delta;
    }
    pulseCount++;
    lastPulseTime = now;
  }
}

static const uint32_t crc32_tab[] = {
  0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
  0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
  0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
  0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
  0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
  0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
  0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
  0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d
};

uint32_t calc_crc32(File &f) {
  uint32_t crc = 0xFFFFFFFF;
  f.seek(0);
  uint8_t buf[256];
  while (f.available()) {
    size_t len = f.read(buf, sizeof(buf));
    for (size_t i = 0; i < len; i++) {
      crc = (crc >> 8) ^ crc32_tab[(crc ^ buf[i]) & 0xFF];
    }
  }
  return crc ^ 0xFFFFFFFF;
}

// G.711 u-law (PCMU) Audio Codec (2:1 Kompression fuer WAV)
// Reduziert Datenrate von 32 KB/s auf 16 KB/s bei voller 16 kHz Sprachqualitaet
static const uint8_t pcmToUlawTable[256] = {
  0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
  5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
  6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
  6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

uint8_t linear2ulaw(int16_t pcm_val) {
  int mask;
  int seg;
  uint8_t uval;
  if (pcm_val < 0) {
    pcm_val = -pcm_val;
    mask = 0x7F;
  } else {
    mask = 0xFF;
  }
  if (pcm_val > 8159) pcm_val = 8159;
  pcm_val += 0x84;
  seg = pcmToUlawTable[(pcm_val >> 6) & 0xFF];
  uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F);
  return (uval ^ mask);
}

int16_t ulaw2linear(uint8_t u_val) {
  int t;
  u_val = ~u_val;
  t = ((u_val & 0x0F) << 3) + 0x84;
  t <<= (u_val & 0x70) >> 4;
  return ((u_val & 0x80) ? (0x84 - t) : (t - 0x84));
}

bool isHandsetOffHook() {
  return digitalRead(HOOK_PIN) == LOW;
}

void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_stop(I2S_NUM_0); // Standardmäßig I2S Takt stoppen -> Absolute Ruhe im Lautsprecher!
}

bool i2sRunning = false;
void enableAudioOutput() {
  if (!i2sRunning) {
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM); // WLAN-Radio schlafen legen
    i2s_start(I2S_NUM_0);
    i2sRunning = true;
    delay(10);
  }
}

void disableAudioOutput() {
  if (i2sRunning) {
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(20);
    i2s_stop(I2S_NUM_0);
    i2sRunning = false;
    esp_wifi_set_ps(WIFI_PS_NONE); // WLAN wieder voll aufwecken
  }
}

inline int16_t scaleSample(int16_t val) {
  int32_t s = (int32_t)val * currentVolume / 100;
  if (s > 32767) s = 32767;
  if (s < -32768) s = -32768;
  return (int16_t)s;
}

bool play_tone(float freq, int durationMs) {
  enableAudioOutput();
  const int total_samples = SAMPLE_RATE * (durationMs / 1000.0);
  float phase = 0.0;
  float step = 2.0 * M_PI * freq / SAMPLE_RATE;
  int16_t buf[512 * 2];
  int written_samples = 0;

  while (written_samples < total_samples) {
    if (!isHandsetOffHook()) {
      disableAudioOutput();
      return false;
    }
    int chunk = min(512, total_samples - written_samples);
    for (int i = 0; i < chunk; i++) {
      int16_t val = scaleSample((int16_t)(sin(phase) * 12000));
      buf[i * 2]     = val;
      buf[i * 2 + 1] = val;
      phase += step;
      if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    size_t written;
    i2s_write(I2S_NUM_0, buf, chunk * 4, &written, portMAX_DELAY);
    written_samples += chunk;
  }
  disableAudioOutput();
  return true;
}

bool play_beep() {
  return play_tone(1000.0, 400);
}

// Deutscher Besetztton (425 Hz, 480ms Ton / 480ms Pause)
void play_busy_tone(int cycles = 4) {
  for (int i = 0; i < cycles; i++) {
    if (!play_tone(425.0, 480)) break;
    delay(480);
  }
}

bool play_wav_file(const char* path, bool checkHook = true, bool abortOnDial = false) {
  fs::FS* fs = storageFS->exists(path) ? storageFS : &LittleFS;
  if (!fs->exists(path)) return false;
  File f = fs->open(path, "r");
  if (!f) return false;

  uint8_t hdr[44];
  if (f.read(hdr, 44) < 44) {
    f.close();
    return false;
  }
  uint16_t audioFormat = hdr[20] | (hdr[21] << 8); // 1 = PCM, 7 = u-law (PCMU)

  enableAudioOutput();
  int16_t out_buf[512 * 2];

  if (audioFormat == 7) {
    // G.711 u-law Wiedergabe (Abwärtskompatibel für alte Aufnahmen)
    uint8_t in_buf[256];
    while (f.available()) {
      if (checkHook && !isHandsetOffHook()) { f.close(); disableAudioOutput(); return false; }
      if (abortOnDial && (pulseCount > 0 || digitalRead(BUTTON_PIN) == LOW)) {
        if (digitalRead(BUTTON_PIN) == LOW) buttonPressDetected = true;
        i2s_zero_dma_buffer(I2S_NUM_0);
        f.close();
        disableAudioOutput();
        return false;
      }
      size_t bytes_read = f.read(in_buf, sizeof(in_buf));
      for (size_t i = 0; i < bytes_read; i++) {
        int16_t linear = ulaw2linear(in_buf[i]);
        int16_t val = scaleSample(linear);
        out_buf[i * 2]     = val;
        out_buf[i * 2 + 1] = val;
      }
      size_t written;
      i2s_write(I2S_NUM_0, out_buf, bytes_read * 4, &written, portMAX_DELAY);
    }
  } else {
    // 16-Bit PCM Wiedergabe (Standard Studio-Qualität)
    int16_t in_buf[256];
    while (f.available()) {
      if (checkHook && !isHandsetOffHook()) { f.close(); disableAudioOutput(); return false; }
      if (abortOnDial && (pulseCount > 0 || digitalRead(BUTTON_PIN) == LOW)) {
        if (digitalRead(BUTTON_PIN) == LOW) buttonPressDetected = true;
        i2s_zero_dma_buffer(I2S_NUM_0);
        f.close();
        disableAudioOutput();
        return false;
      }
      size_t bytes_read = f.read((uint8_t*)in_buf, sizeof(in_buf));
      size_t samples = bytes_read / 2;
      for (size_t i = 0; i < samples; i++) {
        int16_t val = scaleSample(in_buf[i]);
        out_buf[i * 2]     = val;
        out_buf[i * 2 + 1] = val;
      }
      size_t written;
      i2s_write(I2S_NUM_0, out_buf, samples * 4, &written, portMAX_DELAY);
    }
  }
  f.close();
  disableAudioOutput();
  return true;
}

bool play_wav_memory(const unsigned char* wav_data, size_t wav_len, bool checkHook = true, bool abortOnDial = false) {
  size_t data_offset = 44;
  size_t search_limit = (wav_len > 8192) ? 8192 : (wav_len - 8);
  for (size_t i = 0; i < search_limit; i++) {
    if (wav_data[i] == 'd' && wav_data[i+1] == 'a' && 
        wav_data[i+2] == 't' && wav_data[i+3] == 'a') {
      data_offset = i + 8;
      break;
    }
  }

  enableAudioOutput();
  const int16_t* pcm_data = (const int16_t*)(wav_data + data_offset);
  size_t total_samples = (wav_len - data_offset) / 2;
  int16_t out_buf[256 * 2];
  size_t idx = 0;

  while (idx < total_samples) {
    if (checkHook && !isHandsetOffHook()) { disableAudioOutput(); return false; }
    // Nur abbrechen, wenn explizit gewünscht (Drehen oder Erdtaste gedrückt)
    if (abortOnDial && (pulseCount > 0 || digitalRead(BUTTON_PIN) == LOW)) {
      if (digitalRead(BUTTON_PIN) == LOW) buttonPressDetected = true;
      i2s_zero_dma_buffer(I2S_NUM_0);
      disableAudioOutput();
      return false;
    }

    int chunk = min((size_t)256, total_samples - idx);
    for (int i = 0; i < chunk; i++) {
      int16_t val = scaleSample(pcm_data[idx++]);
      out_buf[i * 2]     = val;
      out_buf[i * 2 + 1] = val;
    }
    size_t written;
    i2s_write(I2S_NUM_0, out_buf, chunk * 4, &written, portMAX_DELAY);
  }
  disableAudioOutput();
  return true;
}

bool play_greeting(bool checkHook = true, bool abortOnDial = true) {
  if (storageFS->exists("/greeting.wav") || LittleFS.exists("/greeting.wav")) {
    return play_wav_file("/greeting.wav", checkHook, abortOnDial);
  }
  return play_wav_memory(greeting_wav, greeting_wav_len, checkHook, abortOnDial);
}

bool play_easter1(bool checkHook = true) {
  pulseCount = 0; // Prell-Impulse vor Abspielen löschen
  fs::FS* fs = storageFS->exists("/easter_1.wav") ? storageFS : &LittleFS;
  if (fs->exists("/easter_1.wav")) {
    File f = fs->open("/easter_1.wav", "r");
    if (f && f.size() > 44) {
      f.close();
      Serial.println("Spiele Easter Egg 1 von Dateisystem (/easter_1.wav)...");
      return play_wav_file("/easter_1.wav", checkHook, true);
    }
    if (f) f.close();
  }
  Serial.println("Spiele Easter Egg 1 aus Flash-Speicher (Witz)...");
  return play_wav_memory(default_easter1_wav, default_easter1_wav_len, checkHook, true);
}

bool play_easter2(bool checkHook = true) {
  pulseCount = 0; // Prell-Impulse vor Abspielen löschen
  fs::FS* fs = storageFS->exists("/easter_7.wav") ? storageFS : &LittleFS;
  if (fs->exists("/easter_7.wav")) {
    File f = fs->open("/easter_7.wav", "r");
    if (f && f.size() > 44) {
      f.close();
      Serial.println("Spiele Easter Egg 2 von Dateisystem (/easter_7.wav)...");
      return play_wav_file("/easter_7.wav", checkHook, true);
    }
    if (f) f.close();
  }
  Serial.println("Spiele Easter Egg 2 aus Flash-Speicher (Staendchen)...");
  return play_wav_memory(default_easter7_wav, default_easter7_wav_len, checkHook, true);
}

// Letzte Aufnahme abspielen (ohne langsames Öffnen aller Dateien im Ordner!)
void play_last_recording() {
  // Sofortiges akustisches Feedback beim Knopfdruck!
  play_beep();
  delay(100);

  String lastFile = "";
  // Schnellsuche rückwärts ab der aktuellen Aufnahmenummer (dauert < 5 ms statt Sekunden!)
  for (int n = nextRecordNum - 1; n >= 1; n--) {
    char candidate[32];
    snprintf(candidate, sizeof(candidate), "/recordings/rec_%04d.wav", n);
    if (storageFS->exists(candidate)) {
      lastFile = String(candidate);
      break;
    }
  }

  // Fallback, falls Dateien anders heißen
  if (lastFile.length() == 0) {
    File root = storageFS->open("/recordings");
    if (root && root.isDirectory()) {
      String fname = root.getNextFileName();
      while (fname.length() > 0) {
        if (!fname.startsWith("/")) lastFile = "/recordings/" + fname;
        else lastFile = fname;
        fname = root.getNextFileName();
      }
      root.close();
    }
  }

  if (lastFile.length() > 0) {
    Serial.println("Spiele letzte Aufnahme im Hoerer ab: " + lastFile);
    play_wav_file(lastFile.c_str(), true, true);
  }
}

// Glocken klingeln lassen (mechanische Spule ansteuern)
void ring_bells(int seconds = 4) {
  if (isHandsetOffHook()) return; // Nicht bimmeln, wenn Hörer schon abgenommen ist
  Serial.println("🔔 Bimmele Glocken (Incoming Call)...");
  unsigned long start = millis();
  while (millis() - start < seconds * 1000) {
    if (isHandsetOffHook()) break; // Sofort stoppen beim Abheben!
    // 25 Hz Wechselspannung simulieren (20ms Polaritaet A, 20ms Polaritaet B)
    digitalWrite(BELL_PIN_A, HIGH);
    digitalWrite(BELL_PIN_B, LOW);
    delay(20);
    digitalWrite(BELL_PIN_A, LOW);
    digitalWrite(BELL_PIN_B, HIGH);
    delay(20);
  }
  digitalWrite(BELL_PIN_A, LOW);
  digitalWrite(BELL_PIN_B, LOW);
}

// Unkomprimierter 16-Bit PCM WAV Header (32 KB/s bei 16 kHz)
void writePcmWavHeader(File &file, uint32_t dataLen) {
  file.seek(0);
  uint32_t fileSize = 36 + dataLen;
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t channels = 1;
  uint16_t bitsPerSample = 16; // 16-Bit unkomprimiert
  uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8); // 32000 Bytes/sec
  uint16_t blockAlign = channels * (bitsPerSample / 8);

  file.write((const uint8_t*)"RIFF", 4);
  file.write((const uint8_t*)&fileSize, 4);
  file.write((const uint8_t*)"WAVE", 4);
  file.write((const uint8_t*)"fmt ", 4);
  uint32_t subChunk1Size = 16;
  file.write((const uint8_t*)&subChunk1Size, 4);
  uint16_t audioFormat = 1; // 1 = Unkomprimiertes PCM
  file.write((const uint8_t*)&audioFormat, 2);
  file.write((const uint8_t*)&channels, 2);
  file.write((const uint8_t*)&sampleRate, 4);
  file.write((const uint8_t*)&byteRate, 4);
  file.write((const uint8_t*)&blockAlign, 2);
  file.write((const uint8_t*)&bitsPerSample, 2);
  file.write((const uint8_t*)"data", 4);
  file.write((const uint8_t*)&dataLen, 4);
}

void startRecording() {
  // 1. SOFORT den Mikrofon-Timer & Ringpuffer starten (0 ms Verzögerung nach dem Piepton!),
  //    damit die erste Sekunde während des SD-Karten-Öffnens bereits im RAM gepuffert wird!
  recordDataBytes = 0;
  int currentRaw = analogRead(MIC_PIN);
  if (currentRaw > 200 && currentRaw < 3800) {
    micDcBias = currentRaw; // Sofort auf aktuellen Gleichspannungs-Pegel setzen (kein Einschwing-Knacksen!)
  }
  ringHead = 0;
  ringTail = 0;
  recordStartMillis = millis();
  isRecording = true;

  if (!recTimer) {
    recTimer = timerBegin(1000000); // 1 MHz Takt
    timerAttachInterrupt(recTimer, &onRecordSample);
  }
  timerAlarm(recTimer, 62, true, 0); // 16.129 kHz (exakt ~16 kHz)

  // 2. Dateiname bestimmen & auf SD-Karte anlegen (währenddessen puffert der Timer schon live!)
  if (recordMode == 1) {
    currentRecordFilename = "/greeting.wav";
    Serial.println("Starte Aufnahme: NEUE ANSAGE (/greeting.wav)");
  } else if (recordMode == 2) {
    currentRecordFilename = "/easter_1.wav";
    Serial.println("Starte Aufnahme: EASTER EGG 1 (/easter_1.wav)");
  } else if (recordMode == 3) {
    currentRecordFilename = "/easter_7.wav";
    Serial.println("Starte Aufnahme: EASTER EGG 2 (/easter_7.wav)");
  } else {
    char filename[32];
    snprintf(filename, sizeof(filename), "/recordings/rec_%04d.wav", nextRecordNum++);
    prefs.putInt("nextNum", nextRecordNum);
    currentRecordFilename = String(filename);
    Serial.print("Aufnahme gestartet: ");
    Serial.println(filename);
  }

  currentRecordFile = storageFS->open(currentRecordFilename, "w");
  if (!currentRecordFile) {
    Serial.println("Fehler beim Erstellen der Datei!");
    isRecording = false;
    timerAlarm(recTimer, 0, false, 0);
    return;
  }

  uint8_t zeroBuf[44] = {0};
  currentRecordFile.write(zeroBuf, 44);
}

void stopRecording() {
  if (!isRecording) return;
  isRecording = false;

  if (recTimer) {
    timerAlarm(recTimer, 0, false, 0); // Timer stoppen
  }

  // Restliche Samples aus dem Ringpuffer noch in die Datei schreiben
  if (currentRecordFile) {
    int16_t pcmChunk[256];
    while (ringTail != ringHead) {
      int count = 0;
      while (ringTail != ringHead && count < 256) {
        pcmChunk[count++] = ringBuffer[ringTail];
        ringTail = (ringTail + 1) % RING_BUF_SIZE;
      }
      if (count > 0) {
        size_t w = currentRecordFile.write((const uint8_t*)pcmChunk, count * sizeof(int16_t));
        recordDataBytes += w;
      }
    }
  }

  uint32_t durationSec = (millis() - recordStartMillis) / 1000;

  // Pruefe Mindestdauer (nur für normale Gast-Aufnahmen)
  if (recordMode == 0 && durationSec < (uint32_t)minRecordSec) {
    currentRecordFile.close();
    storageFS->remove(currentRecordFilename);
    Serial.printf("Aufnahme zu kurz (%d Sek. < %d Sek.) -> verworfen!\n", durationSec, minRecordSec);
    return;
  }

  writePcmWavHeader(currentRecordFile, recordDataBytes);
  currentRecordFile.close();
  Serial.printf("Aufnahme beendet & gespeichert: %s (%d Sek.)\n", currentRecordFilename.c_str(), durationSec);

  // Modus wieder zurücksetzen
  recordMode = 0;
}

String buildRecordingsTable() {
  File root = storageFS->open("/recordings");
  if (!root || !root.isDirectory()) {
    return "<div class='empty-state'>Keine Aufnahmen vorhanden.</div>";
  }

  String html = "<table><thead><tr><th>Dateiname</th><th>Gr&ouml;&szlig;e</th><th>Dauer</th><th>Anh&ouml;ren / Download</th><th>Aktion</th></tr></thead><tbody>";
  int count = 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      count++;
      String name = String(f.name());
      int slashIdx = name.lastIndexOf('/');
      if (slashIdx >= 0) name = name.substring(slashIdx + 1);
      size_t sz = f.size();
      int durationSec = (sz >= 44) ? ((sz - 44) / 32000) : 0;
      if (durationSec == 0 && sz > 44) durationSec = 1;

      html += "<tr>";
      html += "<td><strong>" + name + "</strong></td>";
      html += "<td>" + String(sz / 1024) + " KB</td>";
      html += "<td>" + String(durationSec) + " Sek.</td>";
      html += "<td>";
      html += "<audio controls src='/download?file=" + name + "' style='height:30px; vertical-align:middle; margin-right:8px;'></audio>";
      html += "<a href='/download?file=" + name + "' class='btn btn-sm' download>💾</a>";
      html += "</td>";
      html += "<td><button class='btn btn-danger btn-sm' onclick='deleteFile(\"" + name + "\")'>🗑️</button></td>";
      html += "</tr>";
    }
    f = root.openNextFile();
  }
  html += "</tbody></table>";
  if (count == 0) {
    return "<div class='empty-state'>Noch keine Aufnahmen vorhanden. Nimm den H&ouml;rer ab, um die erste Nachricht zu testen!</div>";
  }
  return html;
}

int getRecordCount() {
  File root = storageFS->open("/recordings");
  if (!root || !root.isDirectory()) return 0;
  int c = 0;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) c++;
    f = root.openNextFile();
  }
  return c;
}

void handleRoot() {
  uint64_t totalBytes = sdCardMounted ? SD.totalBytes() : LittleFS.totalBytes();
  uint64_t usedBytes  = sdCardMounted ? SD.usedBytes()  : LittleFS.usedBytes();
  uint64_t freeBytes  = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
  int usagePercent    = (totalBytes > 0) ? (usedBytes * 100 / totalBytes) : 0;

  uint32_t remainingSeconds = freeBytes / 32000; // 32 KB/s bei 16-Bit PCM
  String remainingTimeStr = "";
  if (remainingSeconds < 60) {
    remainingTimeStr = "ca. " + String(remainingSeconds) + " Sek. frei";
  } else if (remainingSeconds < 3600) {
    int m = remainingSeconds / 60;
    remainingTimeStr = "ca. " + String(m) + " Min. frei";
  } else {
    int h = remainingSeconds / 3600;
    int m = (remainingSeconds % 3600) / 60;
    remainingTimeStr = "ca. " + String(h) + " Std. " + String(m) + " Min. frei";
  }

  String totalStr = (totalBytes >= 1048576) ? (String(totalBytes / 1048576.0, 1) + " MB") : (String(totalBytes / 1024) + " KB");
  String freeStr = (freeBytes >= 1048576) ? (String(freeBytes / 1048576.0, 1) + " MB") : (String(freeBytes / 1024) + " KB");

  String page = FPSTR(INDEX_HTML);
  if (sdCardMounted) {
    page.replace("Interner Flash (LittleFS).", "MicroSD-Karte (16-Bit PCM Studio-Qualit&auml;t).");
  }
  page.replace("%TOTAL_SPACE%", totalStr);
  page.replace("%FREE_SPACE%", freeStr);
  page.replace("%USAGE_PERCENT%", String(usagePercent));
  page.replace("%REMAINING_TIME%", remainingTimeStr);
  page.replace("%VOLUME%", String(currentVolume));
  page.replace("%MIC_GAIN%", String(micGain));
  page.replace("%GREETING_STATUS%", (storageFS->exists("/greeting.wav") || LittleFS.exists("/greeting.wav")) ? "Eigene Ansage (.wav)" : "Standard-Ansage (integriert)");
  page.replace("%REC_COUNT%", String(getRecordCount()));
  page.replace("%RECORDINGS_TABLE%", buildRecordingsTable());
  page.replace("%SSID%", wifiSSID);
  page.replace("%PASS%", wifiPass);
  page.replace("%WEBHOOK%", webhookURL);
  page.replace("%MIN_SEC%", String(minRecordSec));
  page.replace("%MAX_SEC%", String(maxRecordSec));
  page.replace("%RING_CHECKED%", ringActive ? "checked" : "");
  page.replace("%RING_INTERVAL%", String(ringIntervalMin));
  page.replace("%EASTER1_STATUS%", (storageFS->exists("/easter_1.wav") || LittleFS.exists("/easter_1.wav")) ? "<span class='badge'>Eigene WAV aktiv</span>" : "<span class='badge' style='background:#7f8c8d'>Standard aktiv</span>");
  page.replace("%EASTER7_STATUS%", (storageFS->exists("/easter_7.wav") || LittleFS.exists("/easter_7.wav")) ? "<span class='badge'>Eigene WAV aktiv</span>" : "<span class='badge' style='background:#7f8c8d'>Standard aktiv</span>");
  page.replace("%CODE_LAST%", codeLastRec);
  page.replace("%CODE_RING%", codeRingToggle);
  page.replace("%CODE_ADMIN%", codeAdminPin);
  page.replace("%CODE_E1%", codeEaster1);
  page.replace("%CODE_E2%", codeEaster2);
  page.replace("%CODE_REC_E1%", codeRecEaster1);
  page.replace("%CODE_REC_E2%", codeRecEaster2);
  server.send(200, "text/html", page);
}

void handleSetVolume() {
  if (server.hasArg("vol")) {
    currentVolume = server.arg("vol").toInt();
    if (currentVolume < 10) currentVolume = 10;
    if (currentVolume > 100) currentVolume = 100;
    prefs.putInt("vol", currentVolume);
    Serial.printf("Neue Lautstaerke: %d%%\n", currentVolume);
  }
  server.send(200, "text/plain", "OK");
}

void handleSetMicGain() {
  if (server.hasArg("gain")) {
    micGain = server.arg("gain").toInt();
    if (micGain < 1) micGain = 1;
    if (micGain > 30) micGain = 30;
    prefs.putInt("mic_gain", micGain);
    Serial.printf("Neue Aufnahme-Empfindlichkeit: %dx\n", micGain);
  }
  server.send(200, "text/plain", "OK");
}

void handlePlayTest() {
  String target = server.hasArg("target") ? server.arg("target") : "greeting";
  Serial.println("Probe-Abspielen per Web: " + target);
  if (target == "e1") {
    play_easter1(false);
  } else if (target == "e2") {
    play_easter2(false);
  } else {
    play_greeting(false);
  }
  server.send(200, "text/plain", "OK");
}

String sanitizeCode(String in, const char* fallback) {
  String out = "";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '1' || c == '5' || c == '9') {
      out += c;
    }
  }
  return (out.length() > 0) ? out : String(fallback);
}

void handleSaveCodes() {
  if (server.hasArg("code_last")) {
    codeLastRec = sanitizeCode(server.arg("code_last"), "1");
    prefs.putString("c_last", codeLastRec);
  }
  if (server.hasArg("code_ring")) {
    codeRingToggle = sanitizeCode(server.arg("code_ring"), "9999");
    prefs.putString("c_ring", codeRingToggle);
  }
  if (server.hasArg("code_admin")) {
    codeAdminPin = sanitizeCode(server.arg("code_admin"), "199");
    prefs.putString("c_adm", codeAdminPin);
  }
  if (server.hasArg("code_e1")) {
    codeEaster1 = sanitizeCode(server.arg("code_e1"), "5");
    prefs.putString("c_e1", codeEaster1);
  }
  if (server.hasArg("code_e2")) {
    codeEaster2 = sanitizeCode(server.arg("code_e2"), "9");
    prefs.putString("c_e2", codeEaster2);
  }
  if (server.hasArg("code_rec_e1")) {
    codeRecEaster1 = sanitizeCode(server.arg("code_rec_e1"), "599");
    prefs.putString("c_re1", codeRecEaster1);
  }
  if (server.hasArg("code_rec_e2")) {
    codeRecEaster2 = sanitizeCode(server.arg("code_rec_e2"), "999");
    prefs.putString("c_re2", codeRecEaster2);
  }
  Serial.printf("Codes aktualisiert: Last=%s, Ring=%s, Admin=%s, E1=%s, RecE1=%s, E2=%s, RecE2=%s\n",
    codeLastRec.c_str(), codeRingToggle.c_str(), codeAdminPin.c_str(), codeEaster1.c_str(), codeRecEaster1.c_str(), codeEaster2.c_str(), codeRecEaster2.c_str());
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveEventOptions() {
  if (server.hasArg("min_sec")) {
    minRecordSec = server.arg("min_sec").toInt();
    prefs.putInt("min_sec", minRecordSec);
  }
  if (server.hasArg("max_sec")) {
    maxRecordSec = server.arg("max_sec").toInt();
    prefs.putInt("max_sec", maxRecordSec);
  }
  ringActive = server.hasArg("ring_active");
  prefs.putBool("ring_act", ringActive);

  if (server.hasArg("ring_interval")) {
    ringIntervalMin = server.arg("ring_interval").toInt();
    prefs.putInt("ring_int", ringIntervalMin);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

File uploadFile;
void handleUploadGreeting() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = storageFS->open("/greeting.wav", "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadEaster1() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = storageFS->open("/easter_1.wav", "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadEaster7() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = storageFS->open("/easter_7.wav", "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

void handleResetAudio() {
  if (server.hasArg("type")) {
    String type = server.arg("type");
    if (type == "easter1") {
      storageFS->remove("/easter_1.wav");
      LittleFS.remove("/easter_1.wav");
      Serial.println("Easter Egg 1 auf Standard zurückgesetzt.");
    } else if (type == "easter7") {
      storageFS->remove("/easter_7.wav");
      LittleFS.remove("/easter_7.wav");
      Serial.println("Easter Egg 2 (Ständchen) auf Standard zurückgesetzt.");
    } else if (type == "greeting") {
      storageFS->remove("/greeting.wav");
      LittleFS.remove("/greeting.wav");
      Serial.println("Ansage auf Standard zurückgesetzt.");
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDownload() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Fehlt: file Parameter");
    return;
  }
  String filename = server.arg("file");
  if (!filename.startsWith("/")) filename = "/recordings/" + filename;
  
  fs::FS* fs = storageFS->exists(filename) ? storageFS : &LittleFS;
  if (!fs->exists(filename)) {
    server.send(404, "text/plain", "Datei nicht gefunden");
    return;
  }

  File f = fs->open(filename, "r");
  server.streamFile(f, "audio/wav");
  f.close();
}

void handleDeleteFile() {
  if (server.hasArg("file")) {
    String filename = server.arg("file");
    if (!filename.startsWith("/")) filename = "/recordings/" + filename;
    storageFS->remove(filename);
    Serial.println("Geloescht: " + filename);
  }
  server.send(200, "text/plain", "OK");
}

void handleClearAll() {
  File root = storageFS->open("/recordings");
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) {
      String fname = String(f.name());
      int sIdx = fname.lastIndexOf('/');
      if (sIdx >= 0) fname = fname.substring(sIdx + 1);
      String path = "/recordings/" + fname;
      f.close();
      storageFS->remove(path);
      f = root.openNextFile();
    }
  }
  nextRecordNum = 1;
  prefs.putInt("nextNum", 1);
  Serial.println("Alle Aufnahmen geloescht (Event Reset).");
  server.send(200, "text/plain", "OK");
}

void handleSaveSettings() {
  if (server.hasArg("ssid")) {
    wifiSSID = server.arg("ssid");
    prefs.putString("ssid", wifiSSID);
  }
  if (server.hasArg("pass")) {
    wifiPass = server.arg("pass");
    prefs.putString("pass", wifiPass);
  }
  if (server.hasArg("webhook")) {
    webhookURL = server.arg("webhook");
    prefs.putString("webhook", webhookURL);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDownloadAllZip() {
  File root = storageFS->open("/recordings");
  if (!root || !root.isDirectory()) {
    server.send(404, "text/plain", "Keine Aufnahmen vorhanden");
    return;
  }

  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/zip");
  client.println("Content-Disposition: attachment; filename=\"audio_guestbook_recordings.zip\"");
  client.println("Connection: close");
  client.println();

  struct ZipEntry {
    String name;
    uint32_t size;
    uint32_t crc;
    uint32_t offset;
  };
  std::vector<ZipEntry> entries;
  uint32_t currentOffset = 0;

  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      ZipEntry e;
      String fname = String(f.name());
      int sIdx = fname.lastIndexOf('/');
      if (sIdx >= 0) fname = fname.substring(sIdx + 1);
      e.name = fname;
      e.size = f.size();
      e.crc = calc_crc32(f);
      e.offset = currentOffset;

      uint16_t nameLen = e.name.length();
      uint8_t lfh[30] = {
        0x50, 0x4b, 0x03, 0x04,
        0x14, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        (uint8_t)(e.crc & 0xFF), (uint8_t)((e.crc >> 8) & 0xFF), (uint8_t)((e.crc >> 16) & 0xFF), (uint8_t)((e.crc >> 24) & 0xFF),
        (uint8_t)(e.size & 0xFF), (uint8_t)((e.size >> 8) & 0xFF), (uint8_t)((e.size >> 16) & 0xFF), (uint8_t)((e.size >> 24) & 0xFF),
        (uint8_t)(e.size & 0xFF), (uint8_t)((e.size >> 8) & 0xFF), (uint8_t)((e.size >> 16) & 0xFF), (uint8_t)((e.size >> 24) & 0xFF),
        (uint8_t)(nameLen & 0xFF), (uint8_t)((nameLen >> 8) & 0xFF),
        0x00, 0x00
      };

      client.write(lfh, 30);
      client.write((const uint8_t*)e.name.c_str(), nameLen);
      currentOffset += 30 + nameLen;

      f.seek(0);
      uint8_t buffer[512];
      while (f.available()) {
        size_t r = f.read(buffer, sizeof(buffer));
        client.write(buffer, r);
        currentOffset += r;
      }
      entries.push_back(e);
    }
    f = root.openNextFile();
  }

  uint32_t cdStart = currentOffset;
  for (auto &e : entries) {
    uint16_t nameLen = e.name.length();
    uint8_t cdh[46] = {
      0x50, 0x4b, 0x01, 0x02,
      0x14, 0x00,
      0x14, 0x00,
      0x00, 0x00,
      0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      (uint8_t)(e.crc & 0xFF), (uint8_t)((e.crc >> 8) & 0xFF), (uint8_t)((e.crc >> 16) & 0xFF), (uint8_t)((e.crc >> 24) & 0xFF),
      (uint8_t)(e.size & 0xFF), (uint8_t)((e.size >> 8) & 0xFF), (uint8_t)((e.size >> 16) & 0xFF), (uint8_t)((e.size >> 24) & 0xFF),
      (uint8_t)(e.size & 0xFF), (uint8_t)((e.size >> 8) & 0xFF), (uint8_t)((e.size >> 16) & 0xFF), (uint8_t)((e.size >> 24) & 0xFF),
      (uint8_t)(nameLen & 0xFF), (uint8_t)((nameLen >> 8) & 0xFF),
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00,
      0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      (uint8_t)(e.offset & 0xFF), (uint8_t)((e.offset >> 8) & 0xFF), (uint8_t)((e.offset >> 16) & 0xFF), (uint8_t)((e.offset >> 24) & 0xFF)
    };
    client.write(cdh, 46);
    client.write((const uint8_t*)e.name.c_str(), nameLen);
    currentOffset += 46 + nameLen;
  }
  uint32_t cdSize = currentOffset - cdStart;

  uint16_t totalEntries = entries.size();
  uint8_t eocd[22] = {
    0x50, 0x4b, 0x05, 0x06,
    0x00, 0x00, 0x00, 0x00,
    (uint8_t)(totalEntries & 0xFF), (uint8_t)((totalEntries >> 8) & 0xFF),
    (uint8_t)(totalEntries & 0xFF), (uint8_t)((totalEntries >> 8) & 0xFF),
    (uint8_t)(cdSize & 0xFF), (uint8_t)((cdSize >> 8) & 0xFF), (uint8_t)((cdSize >> 16) & 0xFF), (uint8_t)((cdSize >> 24) & 0xFF),
    (uint8_t)(cdStart & 0xFF), (uint8_t)((cdStart >> 8) & 0xFF), (uint8_t)((cdStart >> 16) & 0xFF), (uint8_t)((cdStart >> 24) & 0xFF),
    0x00, 0x00
  };
  client.write(eocd, 22);
}

void startWiFiNetwork() {
  if (wifiSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 14) {
      if (isHandsetOffHook()) break; // Nicht warten, falls Hörer abgenommen wird!
      delay(400);
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("WLAN verbunden (kein störender AP-Beacon!): ");
      Serial.println(WiFi.localIP());
      MDNS.begin("audio-guestbook");
      return;
    }
  }
  // Fallback: AP+STA wenn kein Heim-WLAN verbunden
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.softAP("W48-Guestbook", "guestbook123");
  MDNS.begin("audio-guestbook");
}

void setup() {
  Serial.begin(115200);
  pinMode(HOOK_PIN, INPUT_PULLUP);
  pinMode(DIAL_PULSE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DIAL_PULSE_PIN), onDialPulse, FALLING);

  pinMode(MIC_PIN, INPUT);
  analogReadResolution(12);

  pinMode(BELL_PIN_A, OUTPUT);
  pinMode(BELL_PIN_B, OUTPUT);
  digitalWrite(BELL_PIN_A, LOW);
  digitalWrite(BELL_PIN_B, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount fehlgeschlagen!");
  }
  if (!LittleFS.exists("/recordings")) {
    LittleFS.mkdir("/recordings");
  }

  // MicroSD-Karte initialisieren (CS=10, MOSI=13, SCK=12, MISO=11)
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 1000000, "/sd", 5, true)) {
    Serial.println("SD-Mount fehlgeschlagen -> Repariere Partitionstabelle (Sektor 0) und formatiere FAT32...");
    uint8_t pdrv = sdcard_init(SD_CS, &SPI, 1000000);
    if (pdrv != 0xFF) {
      if (disk_initialize(pdrv) == 0) {
        uint8_t zeroBuf[512] = {0};
        sd_write_raw(pdrv, zeroBuf, 0);
      }
      sdcard_uninit(pdrv);
    }
    delay(100);
    SD.begin(SD_CS, SPI, 1000000, "/sd", 5, true);
  }
  if (SD.cardType() != CARD_NONE && SD.totalBytes() > 0) {
    sdCardMounted = true;
    storageFS = &SD;
    if (!SD.exists("/recordings")) {
      SD.mkdir("/recordings");
    }
    Serial.printf("✅ MicroSD gemountet! Frei: %llu MB von %llu MB\n",
                  (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024),
                  SD.totalBytes() / (1024 * 1024));
  } else {
    sdCardMounted = false;
    storageFS = &LittleFS;
    Serial.println("⚠️ Keine SD-Karte erkannt -> Nutze internen Flash (LittleFS).");
  }

  prefs.begin("guestbook", false);
  currentVolume = prefs.getInt("vol", 50);
  micGain = prefs.getInt("mic_gain", 10);
  nextRecordNum = prefs.getInt("nextNum", 1);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  webhookURL = prefs.getString("webhook", "");
  minRecordSec = prefs.getInt("min_sec", 2);
  maxRecordSec = prefs.getInt("max_sec", 180);
  ringActive = prefs.getBool("ring_act", false);
  ringIntervalMin = prefs.getInt("ring_int", 30);
  codeLastRec = prefs.getString("c_last", "1");
  codeEaster1 = prefs.getString("c_e1", "5");
  codeEaster2 = prefs.getString("c_e2", "9");
  codeAdminPin = prefs.getString("c_adm", "199");
  codeRecEaster1 = prefs.getString("c_re1", "599");
  codeRecEaster2 = prefs.getString("c_re2", "999");
  codeRingToggle = prefs.getString("c_ring", "9999");

  // Migration alter Standard-Codes auf 1/5/9 Raster:
  if (codeLastRec == "0") { codeLastRec = "1"; prefs.putString("c_last", "1"); }
  if (codeEaster1 == "1") { codeEaster1 = "5"; prefs.putString("c_e1", "5"); }
  if (codeEaster2 == "7" || codeEaster2 == "8") { codeEaster2 = "9"; prefs.putString("c_e2", "9"); }
  if (codeAdminPin == "123") { codeAdminPin = "199"; prefs.putString("c_adm", "199"); }
  if (codeRecEaster1 == "111") { codeRecEaster1 = "599"; prefs.putString("c_re1", "599"); }
  if (codeRecEaster2 == "777") { codeRecEaster2 = "999"; prefs.putString("c_re2", "999"); }
  if (codeRingToggle == "99") { codeRingToggle = "9999"; prefs.putString("c_ring", "9999"); }

  Serial.printf("Codes geladen -> LastRec: '%s', E1: '%s', E2: '%s', Admin: '%s', RecE1: '%s', RecE2: '%s', Ring: '%s'\n",
    codeLastRec.c_str(), codeEaster1.c_str(), codeEaster2.c_str(), codeAdminPin.c_str(), codeRecEaster1.c_str(), codeRecEaster2.c_str(), codeRingToggle.c_str());

  initI2S();
  startWiFiNetwork();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/set_volume", HTTP_GET, handleSetVolume);
  server.on("/set_mic_gain", HTTP_GET, handleSetMicGain);
  server.on("/play_test", HTTP_GET, handlePlayTest);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/download_all", HTTP_GET, handleDownloadAllZip);
  server.on("/delete_file", HTTP_POST, handleDeleteFile);
  server.on("/clear_all", HTTP_POST, handleClearAll);
  server.on("/save_settings", HTTP_POST, handleSaveSettings);
  server.on("/save_event_options", HTTP_POST, handleSaveEventOptions);
  server.on("/save_codes", HTTP_POST, handleSaveCodes);
  server.on("/reset_audio", HTTP_POST, handleResetAudio);
  server.on("/upload_greeting", HTTP_POST, []() {
    server.sendHeader("Location", "/");
    server.send(303);
  }, handleUploadGreeting);
  server.on("/upload_easter1", HTTP_POST, []() {
    server.sendHeader("Location", "/");
    server.send(303);
  }, handleUploadEaster1);
  server.on("/upload_easter7", HTTP_POST, []() {
    server.sendHeader("Location", "/");
    server.send(303);
  }, handleUploadEaster7);

  server.begin();
  lastRingCheckMillis = millis();
}

bool hasLongerCodeWithPrefix(const String& prefix) {
  if (codeLastRec.length() > prefix.length() && codeLastRec.startsWith(prefix)) return true;
  if (codeEaster1.length() > prefix.length() && codeEaster1.startsWith(prefix)) return true;
  if (codeEaster2.length() > prefix.length() && codeEaster2.startsWith(prefix)) return true;
  if (codeAdminPin.length() > prefix.length() && codeAdminPin.startsWith(prefix)) return true;
  if (codeRecEaster1.length() > prefix.length() && codeRecEaster1.startsWith(prefix)) return true;
  if (codeRecEaster2.length() > prefix.length() && codeRecEaster2.startsWith(prefix)) return true;
  if (codeRingToggle.length() > prefix.length() && codeRingToggle.startsWith(prefix)) return true;
  return false;
}

unsigned long getDynamicTimeout(const String& code) {
  if (code.length() == 0) return 2000;
  char lastDigit = code[code.length() - 1];
  if (lastDigit == '1') return 1200; // Sehr kurzer Weg (30°): 1,2 Sek.
  if (lastDigit == '5') return 2200; // Mittlerer Weg (150°): 2,2 Sek.
  if (lastDigit == '9') return 4000; // Großer Winkel (270°): 4,0 Sek.
  return 2500;
}

void handleDialAction(const String& code) {
  Serial.println("Eingegebener Code: " + code);
  if (code.length() == 0) return;

  if (code == codeLastRec) {
    // Letzte Aufnahme abhören
    if (isRecording) stopRecording();
    play_last_recording();
  } else if (code == codeEaster1) {
    // Easter Egg 1 (z. B. Witz anhören)
    if (isRecording) stopRecording();
    play_beep();
    play_easter1(true);
  } else if (code == codeEaster2) {
    // Easter Egg 2 (z. B. Ständchen / Musik anhören)
    if (isRecording) stopRecording();
    play_beep();
    play_easter2(true);
  } else if (code == codeRecEaster1) {
    // Easter Egg 1 NEU aufsprechen
    if (isRecording) stopRecording();
    recordMode = 2;
    play_beep();
    delay(200);
    play_beep();
    Serial.printf("Code %s erkannt: Bereit zum Aufsprechen von Witz / Easter Egg 1!\n", codeRecEaster1.c_str());
    startRecording();
  } else if (code == codeRecEaster2) {
    // Easter Egg 2 NEU aufsprechen
    if (isRecording) stopRecording();
    recordMode = 3;
    play_beep();
    delay(200);
    play_beep();
    Serial.printf("Code %s erkannt: Bereit zum Aufsprechen von Ständchen / Easter Egg 2!\n", codeRecEaster2.c_str());
    startRecording();
  } else if (code == codeRingToggle) {
    // Zufalls-Bimmeln Umschalten (An / Aus)
    if (isRecording) stopRecording();
    ringActive = !ringActive;
    prefs.putBool("ring_act", ringActive);
    Serial.printf("Zufallsklingeln umgeschaltet: %s\n", ringActive ? "AN" : "AUS");

    if (ringActive) {
      Serial.println("Ansage: Zufall an");
      play_wav_memory(ring_on_wav, ring_on_wav_len, true, true);
    } else {
      Serial.println("Ansage: Zufall aus");
      play_wav_memory(ring_off_wav, ring_off_wav_len, true, true);
    }
  } else if (code == codeAdminPin) {
    // Admin PIN - Neue Ansage aufsprechen
    if (isRecording) stopRecording();
    recordMode = 1;
    play_beep();
    delay(200);
    play_beep();
    Serial.printf("Admin PIN %s erkannt: Bereit zum Aufsprechen neuer Ansage!\n", codeAdminPin.c_str());
    startRecording();
  } else {
    Serial.println("Unbekannter Code gewählt.");
  }
}

void loop() {
  if (!wasOffHook) {
    server.handleClient();
  }

  bool offHook = isHandsetOffHook();

  // Waehlscheiben-Erkennung mit Toleranz-Raster (1, 5, 9)
  if (pulseCount > 0 && (millis() - lastPulseTime > 200)) {
    int rawPulses = pulseCount;
    pulseCount = 0;

    int mappedDigit = -1;
    if (rawPulses >= 1 && rawPulses <= 3) {
      mappedDigit = 1;
    } else if (rawPulses >= 4 && rawPulses <= 6) {
      mappedDigit = 5;
    } else if (rawPulses >= 7 && rawPulses <= 12) {
      mappedDigit = 9;
    }

    if (mappedDigit != -1) {
      dialedCode += String(mappedDigit);
      lastDialActivity = millis();
      Serial.printf("Impulse: %d -> Ziffer erkannt: %d (Aktueller Code: %s)\n", rawPulses, mappedDigit, dialedCode.c_str());

      // Sofort ausführen, wenn ein gültiger Code erreicht ist UND kein längerer Code mehr folgen kann (z. B. 199, 599, 9999):
      bool isMatch = (dialedCode == codeLastRec || dialedCode == codeEaster1 || dialedCode == codeEaster2 ||
                      dialedCode == codeAdminPin || dialedCode == codeRecEaster1 || dialedCode == codeRecEaster2 || dialedCode == codeRingToggle);
      bool hasLonger = hasLongerCodeWithPrefix(dialedCode);
      Serial.printf("Code-Check fuer '%s': Match=%d, LaengerMoeglich=%d\n", dialedCode.c_str(), isMatch, hasLonger);
      if (isMatch && !hasLonger) {
        Serial.println("-> Sofort-Aktion gestartet!");
        handleDialAction(dialedCode);
        dialedCode = "";
      }
    } else {
      Serial.printf("Stoerung/Prellen ignoriert: %d Impulse gezaehlt\n", rawPulses);
    }
  }

  // Ausführen nach dynamischer Inaktivität (1: 1,2s | 5: 2,2s | 9: 4,0s)
  unsigned long dynTimeout = getDynamicTimeout(dialedCode);
  if (dialedCode.length() > 0 && (millis() - lastDialActivity > dynTimeout) && pulseCount == 0) {
    Serial.printf("-> Timeout (%lu ms) fuer '%s' erreicht, Aktion gestartet!\n", dynTimeout, dialedCode.c_str());
    handleDialAction(dialedCode);
    dialedCode = "";
  }

  // Taster (Erdtaste auf Pin 9, Schließer nach GND)
  bool btnReading = digitalRead(BUTTON_PIN);
  if (btnReading != lastButtonState) {
    if (millis() - lastButtonDebounce > 50) { // 50 ms Entprellung
      lastButtonState = btnReading;
      lastButtonDebounce = millis();
      if (btnReading == LOW) { // Gedrückt
        Serial.println("🔘 Taster gedrueckt: Letzte Aufnahme abspielen!");
        if (isRecording) {
          stopRecording();
        }
        play_last_recording();
      }
    }
  }

  // Zufalls-Klingeln (nur wenn Hoerer aufliegt)
  if (ringActive && !offHook) {
    unsigned long intervalMs = (unsigned long)ringIntervalMin * 60000;
    if (millis() - lastRingCheckMillis > intervalMs) {
      lastRingCheckMillis = millis();
      ring_bells(4);
    }
  } else if (offHook) {
    lastRingCheckMillis = millis(); // Reset wenn abgenommen
  }

  // Hoerer wird abgenommen -> WLAN komplett AUS fuer 100% stoerungsfreien Ton!
  if (offHook && !wasOffHook) {
    wasOffHook = true;
    pulseCount = 0;
    dialedCode = "";
    buttonPressDetected = false;
    Serial.println("📞 Hoerer abgenommen -> Schalte WLAN komplett aus fuer glasklaren Ton!");
    WiFi.mode(WIFI_OFF);
    delay(150);

    // Begrüßung abspielen (bricht sofort ab, sobald gedreht oder der Knopf gedrückt wird)
    if (play_greeting(true, true) && pulseCount == 0 && dialedCode.length() == 0 && !buttonPressDetected && digitalRead(BUTTON_PIN) == HIGH) {
      delay(200);
      if (play_beep() && pulseCount == 0 && dialedCode.length() == 0 && !buttonPressDetected && digitalRead(BUTTON_PIN) == HIGH) {
        startRecording();
      }
    } else if (buttonPressDetected || digitalRead(BUTTON_PIN) == LOW) {
      buttonPressDetected = false;
      Serial.println("🔘 Taster waehrend Ansage gedrueckt: Letzte Aufnahme abspielen!");
      lastButtonState = digitalRead(BUTTON_PIN);
      lastButtonDebounce = millis();
      play_last_recording();
    }
  }

  // Aufnahme laeuft (Unkomprimiertes 16-Bit PCM, 512-Byte Blöcke für SD-Karte)
  if (isRecording && offHook) {
    // Pruefe Maximaldauer
    if ((millis() - recordStartMillis) / 1000 >= (uint32_t)maxRecordSec) {
      Serial.println("Maximale Aufnahmezeit erreicht!");
      stopRecording();
      play_busy_tone(4); // Besetztton abspielen
    } else {
      // Ringpuffer leeren und in 512-Byte Blöcken (256 Samples à 2 Bytes) auf SD schreiben
      uint32_t head = ringHead;
      uint32_t available = (head >= ringTail) ? (head - ringTail) : (RING_BUF_SIZE - ringTail + head);

      if (available >= 256 && currentRecordFile) {
        int16_t pcmChunk[256];
        for (int i = 0; i < 256; i++) {
          pcmChunk[i] = ringBuffer[ringTail];
          ringTail = (ringTail + 1) % RING_BUF_SIZE;
        }
        size_t w = currentRecordFile.write((const uint8_t*)pcmChunk, sizeof(pcmChunk));
        if (w == 0) {
          Serial.println("Speicher voll! Beende Aufnahme.");
          stopRecording();
        } else {
          recordDataBytes += w;
        }
      }
    }
  }

  // Hoerer wird aufgelegt -> WLAN wieder einschalten!
  if (!offHook && wasOffHook) {
    wasOffHook = false;
    dialedCode = "";
    pulseCount = 0;
    if (isRecording) {
      stopRecording();
    }
    Serial.println("☎️ Hoerer aufgelegt -> Schalte WLAN wieder ein!");
    startWiFiNetwork();
    delay(150);
  }

  delay(5);
}
