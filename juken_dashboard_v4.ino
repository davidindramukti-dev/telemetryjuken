/*
  ============================================================
  JUKEN ECU DASHBOARD  v4.0  —  ESP32 DevKit + Cloud Telemetry
  - TFT ST7789 240x240 (display lokal)
  - WiFi STA → connect ke router/hotspot untuk internet
  - HTTP POST ke Railway server setiap 300ms (telemetry)
  - Session timer mulai saat RPM pertama > 0
  ============================================================
  Library:
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
    - ArduinoJson by Benoit Blanchon   ← tambahan baru

  Board  : ESP32 Dev Module
  Tools  :
    - CPU Frequency   : 240MHz
    - Partition Scheme: Default 4MB
  ============================================================
*/

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ================== WIFI — isi sesuai jaringan kamu ==================
#define WIFI_SSID   "NAMA_WIFI_KAMU"
#define WIFI_PASS   "PASSWORD_WIFI_KAMU"

// ================== SERVER RAILWAY ==================
// Ganti dengan URL Railway kamu setelah deploy
// Contoh: "https://juken-telemetry.up.railway.app"
#define SERVER_URL  "https://NAMA_APP_KAMU.up.railway.app"
#define API_ENDPOINT SERVER_URL "/api/data"

// Interval POST ke server (ms)
#define TELEMETRY_INTERVAL 300

// ================== TFT PIN (ESP32 DevKit — VSPI) ==================
#define TFT_SCLK  18
#define TFT_MOSI  23
#define TFT_DC     2
#define TFT_RST    4
#define TFT_CS    -1

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ================== UART JUKEN ==================
#define RX_JUKEN   16
#define TX_JUKEN   17
#define JUKEN_BAUD 57600

const char* CMD1 = "160A";
const char* CMD2 = "1617";

#define POLL_INTERVAL 300
#define CMD_DELAY_MS  20

String        bufJuken    = "";
unsigned long lastPollTime    = 0;
unsigned long lastTelemetryMs = 0;

// ================== WARNA TFT ==================
#define C_BG      0x0000
#define C_GRID    0x0821
#define C_RED     0xF800
#define C_RED_DIM 0x6000
#define C_CYAN    0x07FF
#define C_CYD     0x0310
#define C_ORANGE  0xFD20
#define C_YELLOW  0xFFE0
#define C_GREEN   0x07E0
#define C_PINK    0xF81F
#define C_GRAY    0x8410
#define C_DGRAY   0x2104

// ================== DATA ECU ==================
float tps      = 0;
float voltage  = 0;
int   rpm      = 0;
float ect      = 0;
float o2v      = 0;
float mapVal   = 0;
int   fuelPump = 0;

// ================== LAST VALUE (anti redraw TFT) ==================
int   lastRPM = -1;
float lastTPS = -999, lastV = -999, lastECT = -999;
float lastO2  = -999, lastMAP = -999;
int   lastFP  = -1;

// ================== SESSION TIMER ==================
bool          sessionStarted = false;
unsigned long sessionStartMs = 0;
unsigned long lastTimerSec   = 999;

// ================== STATUS WIFI & CLOUD ==================
bool wifiOk  = false;
bool cloudOk = false;

// ================== TFT HELPERS ==================
void drawArc(int cx, int cy, int r, int w, float s, float e, uint16_t c) {
  float st = 2.0f * (M_PI / 180.0f);
  float sa = s * (M_PI / 180.0f), ea = e * (M_PI / 180.0f);
  for (float a = sa; a <= ea; a += st)
    for (int i = 0; i < w; i++)
      tft.drawPixel(cx + (int)((r-i)*sin(a)), cy - (int)((r-i)*cos(a)), c);
}

void drawHBar(int x, int y, int w, int h, float pct, uint16_t col, uint16_t trk) {
  pct = constrain(pct, 0, 100);
  int f = (int)((w * pct) / 100.0f);
  tft.fillRect(x, y, f, h, col);
  if (f < w) tft.fillRect(x+f, y, w-f, h, trk);
}

// ================== SEND CMD ==================
void sendSequence() {
  Serial2.println(CMD1); delay(CMD_DELAY_MS);
  Serial2.println(CMD2);
}

// ================== STATUS INDIKATOR TFT ==================
// Pojok kiri bawah: dot WiFi + dot Cloud
void updateStatusDots() {
  // WiFi dot
  tft.fillRect(2, 226, 8, 8, wifiOk  ? C_GREEN : C_RED);
  // Cloud dot
  tft.fillRect(14, 226, 8, 8, cloudOk ? C_GREEN : C_GRAY);

  tft.setTextSize(1);
  tft.setTextColor(C_CYD);
  tft.fillRect(26, 226, 160, 10, C_BG);
  tft.setCursor(26, 227);
  tft.print(wifiOk ? WiFi.localIP().toString() : "NO WIFI");
  if (wifiOk && !cloudOk) { tft.print(" NO SRV"); }
}

// ================== LAYOUT STATIS TFT ==================
void drawLayout() {
  tft.fillScreen(C_BG);
  for (int y = 0; y < 240; y += 30) tft.drawFastHLine(0, y, 240, C_GRID);

  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(85, 132); tft.print("x1000 RPM");
  tft.setCursor(4, 143);  tft.print("TPS");
  tft.setCursor(4, 168);  tft.print("ECT");
  tft.setCursor(8,   200); tft.print("VOLT");
  tft.setCursor(90,  200); tft.print("O2");
  tft.setCursor(168, 200); tft.print("MAP");

  tft.setTextColor(C_CYD);
  tft.setCursor(4, 4); tft.print("SESSION");

  drawArc(120, 80, 70, 8, -135, 135, C_RED_DIM);
  for (int i = 0; i <= 8; i++) {
    float deg = -135.0f + (270.0f / 8.0f) * i;
    float rad = deg * (M_PI / 180.0f);
    tft.drawLine(120+(int)(75*sin(rad)), 80-(int)(75*cos(rad)),
                 120+(int)(63*sin(rad)), 80-(int)(63*cos(rad)), C_GRAY);
  }

  tft.drawFastHLine(0, 136, 240, C_CYD);
  tft.drawFastHLine(0, 193, 240, C_CYD);
  tft.drawFastHLine(0, 221, 240, C_CYD);
  tft.drawFastVLine(80,  193, 28, C_CYD);
  tft.drawFastVLine(160, 193, 28, C_CYD);

  updateStatusDots();
}

// ================== UPDATE TFT ==================
void updateRPM() {
  if (rpm == lastRPM) return; lastRPM = rpm;
  tft.fillRect(60, 55, 120, 40, C_BG);
  drawArc(120, 80, 70, 8, -135, 135, C_RED_DIM);
  float endDeg = -135.0f + (270.0f / 8000.0f) * constrain(rpm, 0, 8000);
  if (endDeg > -135.0f) drawArc(120, 80, 70, 8, -135, endDeg, rpm > 4000 ? C_ORANGE : C_RED);
  tft.setTextSize(3);
  tft.setTextColor(rpm > 4000 ? C_ORANGE : C_RED);
  char buf[8]; snprintf(buf, sizeof(buf), "%5d", rpm);
  tft.setCursor(52, 60); tft.print(buf);
  tft.setTextSize(1); tft.setTextColor(C_GRAY);
  tft.setCursor(85, 132); tft.print("x1000 RPM");
}

void updateTPS() {
  if (tps == lastTPS) return; lastTPS = tps;
  drawHBar(26, 141, 210, 7, tps, C_ORANGE, C_DGRAY);
  tft.fillRect(120, 140, 116, 12, C_BG);
  tft.setTextSize(1); tft.setTextColor(C_ORANGE);
  char buf[10]; dtostrf(tps, 5, 1, buf);
  tft.setCursor(180, 141); tft.print(buf); tft.print("%");
}

void updateECT() {
  if (ect == lastECT) return; lastECT = ect;
  drawHBar(26, 168, 210, 7, constrain((ect/120.0f)*100.0f, 0, 100), C_PINK, C_DGRAY);
  tft.fillRect(120, 167, 116, 12, C_BG);
  tft.setTextSize(1); tft.setTextColor(C_PINK);
  char buf[10]; dtostrf(ect, 5, 1, buf);
  tft.setCursor(165, 168); tft.print(buf); tft.print("\xF7""C");
}

void updateVoltage() {
  if (voltage == lastV) return; lastV = voltage;
  tft.fillRect(1, 200, 78, 20, C_BG);
  tft.setTextSize(2); tft.setTextColor(C_CYAN);
  char buf[8]; dtostrf(voltage, 4, 1, buf);
  tft.setCursor(4, 201); tft.print(buf);
  tft.setTextSize(1); tft.setTextColor(C_GRAY);
  tft.setCursor(56, 204); tft.print("V");
}

void updateO2() {
  if (o2v == lastO2) return; lastO2 = o2v;
  tft.fillRect(82, 200, 77, 20, C_BG);
  tft.setTextSize(2); tft.setTextColor(C_YELLOW);
  char buf[8]; dtostrf(o2v, 4, 2, buf);
  tft.setCursor(85, 201); tft.print(buf);
}

void updateMAP() {
  if (mapVal == lastMAP) return; lastMAP = mapVal;
  tft.fillRect(162, 200, 77, 20, C_BG);
  tft.setTextSize(2); tft.setTextColor(0x87FF);
  char buf[8]; dtostrf(mapVal, 4, 1, buf);
  tft.setCursor(165, 201); tft.print(buf);
}

void updateFuelPump() {
  if (fuelPump == lastFP) return; lastFP = fuelPump;
  tft.fillRect(190, 3, 46, 14, fuelPump ? C_GREEN : C_RED);
  tft.setTextSize(1); tft.setTextColor(C_BG);
  tft.setCursor(193, 6);
  tft.print(fuelPump ? " FP:ON" : "FP:OFF");
}

void updateSessionTimer() {
  if (!sessionStarted) return;
  unsigned long el = (millis() - sessionStartMs) / 1000UL;
  if (el == lastTimerSec) return; lastTimerSec = el;
  unsigned int h = el/3600, m = (el%3600)/60, s = el%60;
  tft.fillRect(52, 3, 132, 12, C_BG);
  char buf[12]; snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
  tft.setTextSize(1); tft.setTextColor(C_CYAN);
  tft.setCursor(55, 4); tft.print(buf);
}

void drawDashboard() {
  updateRPM(); updateTPS(); updateECT();
  updateVoltage(); updateO2(); updateMAP();
  updateFuelPump(); updateSessionTimer();
}

// ================== KIRIM KE CLOUD ==================
void sendTelemetry() {
  if (!wifiOk) return;
  if (millis() - lastTelemetryMs < TELEMETRY_INTERVAL) return;
  lastTelemetryMs = millis();

  // Buat JSON payload
  StaticJsonDocument<200> doc;
  doc["rpm"]     = rpm;
  doc["tps"]     = tps;
  doc["voltage"] = voltage;
  doc["ect"]     = ect;
  doc["o2"]      = o2v;
  doc["map"]     = mapVal;
  doc["fp"]      = fuelPump;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(API_ENDPOINT);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(500);  // timeout 500ms agar tidak block loop

  int code = http.POST(payload);
  cloudOk = (code == 200);
  http.end();

  // Update dot status jika berubah
  static bool prevCloudOk = false;
  if (cloudOk != prevCloudOk) {
    prevCloudOk = cloudOk;
    updateStatusDots();
  }
}

// ================== PARSE JUKEN ==================
void parseA603(String frame) {
  char buf[160]; frame.toCharArray(buf, sizeof(buf));
  char* tok = strtok(buf, ";"); int idx = 0;
  while (tok != NULL) {
    switch (idx) {
      case 1:  tps      = atof(tok);       break;
      case 2:  voltage  = atof(tok);       break;
      case 4:  rpm      = atoi(tok);       break;
      case 5:  ect      = atof(tok)/10.0;  break;
      case 6:  o2v      = atof(tok);       break;
      case 7:  mapVal   = atof(tok);       break;
      case 12: fuelPump = atoi(tok);       break;
    }
    tok = strtok(NULL, ";"); idx++;
  }
  if (!sessionStarted && rpm > 0) {
    sessionStarted = true;
    sessionStartMs = millis();
  }
  drawDashboard();
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  Serial2.begin(JUKEN_BAUD, SERIAL_8N1, RX_JUKEN, TX_JUKEN);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 240, SPI_MODE3);
  tft.setRotation(1);

  // Diagnostic warna
  tft.fillScreen(ST77XX_RED);   delay(200);
  tft.fillScreen(ST77XX_GREEN); delay(200);
  tft.fillScreen(ST77XX_BLUE);  delay(200);

  tft.fillScreen(C_BG);
  tft.setTextColor(C_CYAN); tft.setTextSize(2);
  tft.setCursor(20, 60); tft.print("JUKEN DASH");
  tft.setTextSize(1); tft.setTextColor(C_GRAY);
  tft.setCursor(40, 85); tft.print("v4.0  Telemetry");

  // Koneksi WiFi
  tft.setCursor(10, 105); tft.print("Connecting WiFi...");
  tft.setCursor(10, 117); tft.setTextColor(C_CYAN);
  tft.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) {
    delay(300);
    tft.print(".");
  }

  wifiOk = (WiFi.status() == WL_CONNECTED);

  tft.fillRect(0, 100, 240, 50, C_BG);
  tft.setTextSize(1);
  if (wifiOk) {
    tft.setTextColor(C_GREEN);
    tft.setCursor(10, 105); tft.print("WiFi: "); tft.print(WIFI_SSID);
    tft.setCursor(10, 117); tft.print("IP  : "); tft.print(WiFi.localIP().toString());
    tft.setCursor(10, 129); tft.setTextColor(C_CYD);
    tft.print(SERVER_URL);
    Serial.println("WiFi OK: " + WiFi.localIP().toString());
  } else {
    tft.setTextColor(C_RED);
    tft.setCursor(10, 105); tft.print("WiFi GAGAL!");
    tft.setCursor(10, 117); tft.setTextColor(C_GRAY);
    tft.print("Cek SSID/PASS di kode");
    Serial.println("WiFi GAGAL");
  }

  delay(1500);
  drawLayout();

  sendSequence();
  lastPollTime = millis();
}

// ================== LOOP ==================
void loop() {
  // Cek koneksi WiFi — reconnect jika putus
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiOk) {
      wifiOk = false;
      cloudOk = false;
      updateStatusDots();
    }
    WiFi.reconnect();
    delay(500);
    return;
  }
  if (!wifiOk) {
    wifiOk = true;
    updateStatusDots();
  }

  // Poll ECU
  if (millis() - lastPollTime >= POLL_INTERVAL) {
    sendSequence(); lastPollTime = millis();
  }

  // Kirim telemetry ke cloud
  sendTelemetry();

  // Timer TFT
  updateSessionTimer();

  // Baca UART Juken
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n' || c == '\r') {
      if (bufJuken.length() > 0) {
        if (bufJuken.startsWith("a603")) parseA603(bufJuken);
        bufJuken = "";
      }
    } else {
      bufJuken += c;
    }
  }
}
