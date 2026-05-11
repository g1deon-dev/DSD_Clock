// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  DUAL FC-16 DOT-MATRIX DISPLAY — ESP32                                  ║
// ║  Each module = 4 × NH1088AS (8×8) driven by 4 × MAX7219 = 32×8 pixels  ║
// ╠══════════════════════════════════════════════════════════════════════════╣
// ║  MODULE 1 (INFO)  — SPI-A  DIN=23  CLK=18  CS=5                        ║
// ║    Cycles:  Time (HH:MM:SS AM/PM)  →  Date (DOW DD MON YYYY)           ║
// ║             →  Temperature & Weather description                         ║
// ║                                                                          ║
// ║  MODULE 2 (TEXT)  — SPI-B  DIN=19  CLK=17  CS=16                       ║
// ║    • Supabase approved messages scroll right→left, 3 passes each        ║
// ║    • Ultrasonic object < 20 cm → interrupts with "Hello, there!"        ║
// ║                                                                          ║
// ║  Other hardware                                                          ║
// ║    Buzzer      GPIO 26                                                   ║
// ║    US TRIG     GPIO 14    US ECHO    GPIO 27                             ║
// ║    RTC DS3231  I2C SDA=21 SCL=22                                        ║
// ╠══════════════════════════════════════════════════════════════════════════╣
// ║  Libraries (Arduino Library Manager)                                     ║
// ║    MD_MAX72XX  ≥ 3.3.1   by MajicDesigns                                ║
// ║    MD_Parola   ≥ 3.6.1   by MajicDesigns                                ║
// ║    ArduinoJson ≥ 6.21                                                    ║
// ║    NTPClient   ≥ 3.2.1                                                   ║
// ║    RTClib      ≥ 2.1.1   by Adafruit                                    ║
// ╚══════════════════════════════════════════════════════════════════════════╝

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <queue>
#include <set>

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "marimar";
const char* WIFI_PASSWORD = "martin061106";

// ── NTP (UTC+8 Philippine Time) ───────────────────────────────────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 28800, 60000);

// ── RTC DS3231 ────────────────────────────────────────────────────────────────
RTC_DS3231 rtc;
bool rtcAvailable = false;

// ── Alarm ─────────────────────────────────────────────────────────────────────
const int ALARM_HOUR   = 0;
const int ALARM_MINUTE = 10;
bool alarmTriggered = false;

// ── GPIO ──────────────────────────────────────────────────────────────────────
const int BUZZER_PIN = 26;
const int TRIG_PIN   = 14;
const int ECHO_PIN   = 27;

// ── Ultrasonic config ─────────────────────────────────────────────────────────
const int  DETECT_CM       = 20;
const long GREET_MS        = 9000;   // how long greeting scrolls
const long COOLDOWN_MS     = 2500;

enum UltraState { U_IDLE, U_GREETING, U_COOLDOWN };
UltraState    uState      = U_IDLE;
unsigned long uStateStart = 0;

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  DOT-MATRIX SETUP                                                        ║
// ║                                                                          ║
// ║  Hardware type: FC16_HW                                                  ║
// ║    This matches the standard 4-in-1 NH1088AS / 1088AS modules           ║
// ║    sold on AliExpress / Lazada with the MAX7219 driver board.            ║
// ║    If text is mirrored or scrambled try GENERIC_HW or ICSTATION_HW.     ║
// ║                                                                          ║
// ║  NUM_DEVICES = 4  (four 8×8 blocks per physical module → 32×8 total)    ║
// ╚══════════════════════════════════════════════════════════════════════════╝
#define HW_TYPE      MD_MAX72XX::FC16_HW
#define NUM_DEVICES  4          // 4 × NH1088AS per module

// Module 1 — Info (time / date / weather) — uses hardware SPI bus
// ESP32 hardware SPI: MOSI=23, CLK=18  (MISO unused, not connected)
#define MAT1_CS   5
MD_Parola mat1(HW_TYPE, MAT1_CS, NUM_DEVICES);

// Module 2 — Text/message — uses software SPI so pins are fully independent
// Change these three pins freely; avoid strapping pins (0, 2, 12, 15).
#define MAT2_DIN  19
#define MAT2_CLK  17
#define MAT2_CS   16
MD_Parola mat2(HW_TYPE, MAT2_DIN, MAT2_CLK, MAT2_CS, NUM_DEVICES);

// ── Display brightness (0 dim … 15 bright) ────────────────────────────────────
const uint8_t BRIGHTNESS = 5;

// ── Scroll speed: lower number = FASTER scroll (ms per step internally) ───────
const uint8_t INFO_SPEED = 50;   // mat1 scroll speed
const uint8_t MSG_SPEED  = 45;   // mat2 message scroll speed
const uint8_t GREET_SPEED= 55;   // mat2 greeting scroll speed

// ── Info cycle timing ─────────────────────────────────────────────────────────
enum InfoMode { INFO_TIME, INFO_DATE, INFO_WEATHER };
InfoMode      infoMode      = INFO_TIME;
unsigned long infoModeStart = 0;

const unsigned long TIME_HOLD_MS    = 6000;   // static time shown for 6 s
const unsigned long DATE_SCROLL_MS  = 0;       // 0 = wait for scroll to finish
const unsigned long WX_SCROLL_MS    = 0;       // 0 = wait for scroll to finish

bool infoScrollDone = false;    // set true by MD_Parola when scroll completes

// ── Static time display refresh ───────────────────────────────────────────────
int lastSecDisplayed = -1;

// ── Weather (Open-Meteo — free, no API key) ───────────────────────────────────
// ▶ Change these to your actual location coordinates ◀
const float  WX_LAT = 14.5995f;
const float  WX_LON = 120.9842f;

const unsigned long WX_INTERVAL = 600000UL;   // refresh every 10 min
unsigned long lastWxFetch = 0;
float  wxTempC  = 0.0f;
int    wxCode   = 0;
String wxDesc   = "---";
bool   wxValid  = false;

// ── Supabase ──────────────────────────────────────────────────────────────────
const char* SB_URL = "https://vzsvojwsdwgfnwajmpxp.supabase.co";
const char* SB_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
  ".eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InZ6c3ZvandzZHdnZm53YWptcHhwIiwi"
  "cm9sZSI6ImFub24iLCJpYXQiOjE3NzgxMjYwMDIsImV4cCI6MjA5MzcwMjAwMn0"
  ".Ir9hHWDHkXtZXJYLop73cz1pBsgUch-6UgIThKnOqN4";

const unsigned long POLL_MS = 10000UL;
unsigned long lastPoll      = 0;

// ── Message queue for mat2 ────────────────────────────────────────────────────
struct Msg { String id; String text; };
std::queue<Msg> msgQ;
std::set<String> seenIds;

bool   mat2Busy     = false;
bool   mat2Greeting = false;
int    mat2Passes   = 0;       // count scroll passes for the current message
String mat2CurText  = "";

// Static buffer for Parola (must persist while animating)
static char mat2Buf[128];
static char mat1Buf[32];

// ── Lookup tables ──────────────────────────────────────────────────────────────
const char* DAYS[]   = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
const char* MONTHS[] = { "JAN","FEB","MAR","APR","MAY","JUN",
                          "JUL","AUG","SEP","OCT","NOV","DEC" };

// ═════════════════════════════════════════════════════════════════════════════
// Helper: WMO code → short weather description
// ═════════════════════════════════════════════════════════════════════════════
const char* wmoDesc(int c) {
  if (c == 0)       return "Clear Sky";
  if (c == 1)       return "Mainly Clear";
  if (c == 2)       return "Partly Cloudy";
  if (c == 3)       return "Overcast";
  if (c <= 49)      return "Foggy";
  if (c <= 57)      return "Drizzle";
  if (c <= 67)      return "Rainy";
  if (c <= 77)      return "Snow";
  if (c <= 82)      return "Rain Showers";
  if (c <= 86)      return "Snow Showers";
  if (c <= 99)      return "Thunderstorm";
  return "Unknown";
}

// ═════════════════════════════════════════════════════════════════════════════
// RTC / NTP helpers
// ═════════════════════════════════════════════════════════════════════════════
DateTime getNow() {
  if (rtcAvailable) {
    DateTime t = rtc.now();
    if (t.year() > 2020) return t;
  }
  unsigned long ep = timeClient.getEpochTime();
  return (ep > 1000000000UL) ? DateTime(ep) : DateTime(2024, 1, 1, 0, 0, 0);
}

void syncRTC() {
  timeClient.forceUpdate();
  unsigned long ep = timeClient.getEpochTime();
  if (ep <= 1000000000UL) { Serial.println("NTP invalid."); return; }
  if (rtcAvailable) rtc.adjust(DateTime(ep));
  Serial.println("Time synced: " + DateTime(ep).timestamp());
}

// ═════════════════════════════════════════════════════════════════════════════
// String builders for mat1
// ═════════════════════════════════════════════════════════════════════════════
void buildTimeStr(DateTime& t, char* buf, size_t len) {
  int h = t.hour(), m = t.minute(), s = t.second();
  const char* ap = (h < 12) ? "AM" : "PM";
  int h12 = h % 12; if (!h12) h12 = 12;
  snprintf(buf, len, "%02d:%02d:%02d %s", h12, m, s, ap);
}

void buildDateStr(DateTime& t, char* buf, size_t len) {
  snprintf(buf, len, "%s %02d %s %04d",
           DAYS[t.dayOfTheWeek()], t.day(),
           MONTHS[t.month() - 1], t.year());
}

void buildWeatherStr(char* buf, size_t len) {
  if (!wxValid) { snprintf(buf, len, "No weather data"); return; }
  snprintf(buf, len, "%.1fC  %s", wxTempC, wxDesc.c_str());
}

// ═════════════════════════════════════════════════════════════════════════════
// Fetch weather from Open-Meteo (no API key needed)
// ═════════════════════════════════════════════════════════════════════════════
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&current_weather=true"
    "&temperature_unit=celsius",
    WX_LAT, WX_LON);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      wxTempC = doc["current_weather"]["temperature"].as<float>();
      wxCode  = doc["current_weather"]["weathercode"].as<int>();
      wxDesc  = wmoDesc(wxCode);
      wxValid = true;
      Serial.printf("Weather: %.1f°C  %s\n", wxTempC, wxDesc.c_str());
    }
  } else {
    Serial.println("Weather HTTP: " + String(code));
  }
  http.end();
}

// ═════════════════════════════════════════════════════════════════════════════
// Supabase helpers
// ═════════════════════════════════════════════════════════════════════════════
void markDisplayed(const String& id) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SB_URL) + "/rest/v1/messages?id=eq." + id);
  http.addHeader("apikey", SB_KEY);
  http.addHeader("Authorization", String("Bearer ") + SB_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  int c = http.PATCH("{\"displayed\":true}");
  Serial.println("  Mark displayed [" + id + "]: HTTP " + String(c));
  http.end();
}

void pollSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(SB_URL)
    + "/rest/v1/messages"
    + "?status=eq.approved"
    + "&displayed=eq.false"
    + "&select=id,content"
    + "&order=created_at.asc";

  HTTPClient http;
  http.begin(url);
  http.addHeader("apikey", SB_KEY);
  http.addHeader("Authorization", String("Bearer ") + SB_KEY);

  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, http.getString())) {
      for (JsonObject row : doc.as<JsonArray>()) {
        String id  = row["id"].as<String>();
        String txt = row["content"].as<String>();
        if (!seenIds.count(id)) {
          seenIds.insert(id);
          msgQ.push({ id, txt });
          Serial.println("  Queued [" + id + "]: " + txt);
          markDisplayed(id);
        }
      }
    }
  } else {
    Serial.println("Supabase poll HTTP: " + String(code));
  }
  http.end();
}

// ═════════════════════════════════════════════════════════════════════════════
// Mat2 — start a scroll pass
// ═════════════════════════════════════════════════════════════════════════════
void mat2StartScroll(const char* text, bool isGreeting, uint8_t speed) {
  strncpy(mat2Buf, text, sizeof(mat2Buf) - 1);
  mat2Buf[sizeof(mat2Buf) - 1] = '\0';
  mat2.displayClear();
  // PA_SCROLL_LEFT = right-to-left marquee direction
  mat2.displayScroll(mat2Buf, PA_LEFT, PA_SCROLL_LEFT, speed);
  mat2Busy     = isGreeting ? true : true;
  mat2Greeting = isGreeting;
  Serial.println("Mat2 scroll: " + String(text));
}

// ═════════════════════════════════════════════════════════════════════════════
// Mat2 — try to start the next message from queue
// ═════════════════════════════════════════════════════════════════════════════
void mat2TryNextMessage() {
  if (msgQ.empty()) {
    mat2Busy = false;
    mat2.displayClear();
    return;
  }
  Msg next = msgQ.front();
  msgQ.pop();
  mat2CurText = next.text;
  mat2Passes  = 0;
  mat2StartScroll(mat2CurText.c_str(), false, MSG_SPEED);
}

// ═════════════════════════════════════════════════════════════════════════════
// Mat1 — info cycle tick
//   • INFO_TIME:    static display, refreshed each second, held TIME_HOLD_MS
//   • INFO_DATE:    scrolls once, then advances
//   • INFO_WEATHER: scrolls once, then advances
// ═════════════════════════════════════════════════════════════════════════════
void mat1Tick() {
  DateTime now = getNow();

  switch (infoMode) {

    // ── TIME — static, update each second ──────────────────────────────────
    case INFO_TIME: {
      int sec = now.second();
      if (sec != lastSecDisplayed) {
        lastSecDisplayed = sec;
        buildTimeStr(now, mat1Buf, sizeof(mat1Buf));
        mat1.displayClear();
        // PA_NO_EFFECT = instant render (no animation) for clock readability
        mat1.displayText(mat1Buf, PA_CENTER, INFO_SPEED, 0,
                         PA_NO_EFFECT, PA_NO_EFFECT);
      }
      mat1.displayAnimate();   // must be called every loop to sustain static display

      // Switch to date after TIME_HOLD_MS
      if (millis() - infoModeStart >= TIME_HOLD_MS) {
        infoMode      = INFO_DATE;
        infoModeStart = millis();
        infoScrollDone = false;

        buildDateStr(now, mat1Buf, sizeof(mat1Buf));
        mat1.displayClear();
        mat1.displayScroll(mat1Buf, PA_LEFT, PA_SCROLL_LEFT, INFO_SPEED);
        Serial.println("Mat1 → DATE: " + String(mat1Buf));
      }
      break;
    }

    // ── DATE — scroll once, then switch to weather ──────────────────────────
    case INFO_DATE: {
      if (mat1.displayAnimate()) {   // returns true when one pass finishes
        infoMode       = INFO_WEATHER;
        infoModeStart  = millis();
        infoScrollDone = false;

        buildWeatherStr(mat1Buf, sizeof(mat1Buf));
        mat1.displayClear();
        mat1.displayScroll(mat1Buf, PA_LEFT, PA_SCROLL_LEFT, INFO_SPEED);
        Serial.println("Mat1 → WEATHER: " + String(mat1Buf));
      }
      break;
    }

    // ── WEATHER — scroll once, then switch back to time ─────────────────────
    case INFO_WEATHER: {
      if (mat1.displayAnimate()) {
        infoMode         = INFO_TIME;
        infoModeStart    = millis();
        lastSecDisplayed = -1;   // force immediate time redraw
        mat1.displayClear();
        Serial.println("Mat1 → TIME");
      }
      break;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Ultrasonic distance (cm)
// ═════════════════════════════════════════════════════════════════════════════
long getDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  return dur ? (long)(dur * 0.034f / 2.0f) : 999L;
}

// ═════════════════════════════════════════════════════════════════════════════
// Enter ultrasonic state
// ═════════════════════════════════════════════════════════════════════════════
void enterUState(UltraState s) {
  uState      = s;
  uStateStart = millis();

  if (s == U_GREETING) {
    digitalWrite(BUZZER_PIN, HIGH); delay(120); digitalWrite(BUZZER_PIN, LOW);
    mat2StartScroll("Hello, there!   Welcome!", true, GREET_SPEED);
    Serial.println(">> Greeting started");

  } else if (s == U_COOLDOWN) {
    Serial.println(">> Cooldown");

  } else { // U_IDLE
    Serial.println(">> Idle");
    if (mat2Greeting) {
      mat2Greeting = false;
      mat2Busy     = false;
      mat2.displayClear();
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Alarm sequence (beeps + both matrices show alarm text)
// ═════════════════════════════════════════════════════════════════════════════
void soundAlarm() {
  static char alarmBuf1[] = "** ALARM **  Wake up!  Wake up!  ";
  static char alarmBuf2[] = "!! ALARM !!  Wake up!  Wake up!  ";

  mat1.displayClear();
  mat1.displayScroll(alarmBuf1, PA_LEFT, PA_SCROLL_LEFT, 35);
  mat2.displayClear();
  mat2.displayScroll(alarmBuf2, PA_LEFT, PA_SCROLL_LEFT, 35);

  for (int burst = 0; burst < 3; burst++) {
    for (int beep = 0; beep < 4; beep++) {
      mat1.displayAnimate(); mat2.displayAnimate();
      digitalWrite(BUZZER_PIN, HIGH); delay(80);
      digitalWrite(BUZZER_PIN, LOW);  delay(60);
    }
    delay(300);
  }
  digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
  mat1.displayClear(); mat2.displayClear();
}

// ═════════════════════════════════════════════════════════════════════════════
// Boot splash — scroll a string on the given matrix and wait for it to finish
// ═════════════════════════════════════════════════════════════════════════════
void bootSplash(MD_Parola& m, const char* text, uint8_t speed = 50) {
  static char buf[64];
  strncpy(buf, text, 63); buf[63] = '\0';
  m.displayClear();
  m.displayScroll(buf, PA_LEFT, PA_SCROLL_LEFT, speed);
  while (!m.displayAnimate()) { /* spin */ }
  m.displayClear();
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  // Buzzer OFF immediately
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== DUAL NH1088AS MATRIX BOOT ===");

  // ── Ultrasonic ────────────────────────────────────────────────────────────
  pinMode(TRIG_PIN, OUTPUT); digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);

  // ── Matrix 1 init ─────────────────────────────────────────────────────────
  mat1.begin();
  mat1.setIntensity(BRIGHTNESS);
  mat1.setTextAlignment(PA_LEFT);
  mat1.displayClear();

  // ── Matrix 2 init ─────────────────────────────────────────────────────────
  mat2.begin();
  mat2.setIntensity(BRIGHTNESS);
  mat2.setTextAlignment(PA_LEFT);
  mat2.displayClear();

  // ── Boot splash on both ───────────────────────────────────────────────────
  bootSplash(mat1, "  Booting...  ");
  bootSplash(mat2, "  Please wait...  ");

  // ── I2C + RTC ─────────────────────────────────────────────────────────────
  Wire.begin(21, 22);
  delay(300);
  if (rtc.begin(&Wire)) {
    rtcAvailable = true;
    Serial.println("RTC found: " + rtc.now().timestamp());
    if (rtc.lostPower()) Serial.println("RTC lost power — will sync from NTP.");
  } else {
    Serial.println("No RTC — NTP only.");
    bootSplash(mat1, " No RTC  NTP only ");
  }

  // ── WiFi ──────────────────────────────────────────────────────────────────
  bootSplash(mat1, " Connecting WiFi... ");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true); delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wStart < 15000) {
    delay(500); Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    char ipMsg[40];
    snprintf(ipMsg, sizeof(ipMsg), " WiFi OK  IP:%s  ",
             WiFi.localIP().toString().c_str());
    bootSplash(mat1, ipMsg);

    timeClient.begin();
    syncRTC();
    fetchWeather();
    lastWxFetch = millis();
  } else {
    Serial.println("\nWiFi timeout!");
    bootSplash(mat1, " No WiFi!  Offline ");
  }

  // ── Ready ─────────────────────────────────────────────────────────────────
  mat1.displayClear();
  mat2.displayClear();

  // Prime mat1 with the current time (static)
  {
    DateTime now = getNow();
    buildTimeStr(now, mat1Buf, sizeof(mat1Buf));
    mat1.displayText(mat1Buf, PA_CENTER, INFO_SPEED, 0, PA_NO_EFFECT, PA_NO_EFFECT);
    infoMode      = INFO_TIME;
    infoModeStart = millis();
    lastSecDisplayed = now.second();
  }

  enterUState(U_IDLE);
  Serial.println("Setup complete. Running...");
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  static unsigned long lastWifiCheck = 0;
  static unsigned long lastNtpSync   = 0;

  // ── WiFi watchdog ─────────────────────────────────────────────────────────
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost. Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // ── NTP sync (10 s after boot, then every 60 s) ───────────────────────────
  unsigned long ntpInt = lastNtpSync ? 60000UL : 10000UL;
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpSync > ntpInt) {
    lastNtpSync = millis();
    syncRTC();
  }

  // ── Weather refresh every 10 min ──────────────────────────────────────────
  if (WiFi.status() == WL_CONNECTED && millis() - lastWxFetch > WX_INTERVAL) {
    lastWxFetch = millis();
    fetchWeather();
  }

  // ── Supabase poll every 10 s ──────────────────────────────────────────────
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > POLL_MS) {
    lastPoll = millis();
    pollSupabase();
  }

  // ── Alarm check ───────────────────────────────────────────────────────────
  {
    DateTime now = getNow();
    if (!(now.hour() == ALARM_HOUR && now.minute() == ALARM_MINUTE))
      alarmTriggered = false;

    if (now.hour() == ALARM_HOUR && now.minute() == ALARM_MINUTE
        && now.second() < 10 && !alarmTriggered) {
      alarmTriggered = true;
      soundAlarm();
      // Reset both displays after alarm
      infoMode      = INFO_TIME;
      infoModeStart = millis();
      lastSecDisplayed = -1;
      mat2Busy = false; mat2Greeting = false;
      mat2.displayClear();
      enterUState(U_IDLE);
    }
  }

  // ── Mat1: info cycle ──────────────────────────────────────────────────────
  mat1Tick();

  // ── Mat2: message / greeting ──────────────────────────────────────────────
  if (mat2Busy) {
    bool done = mat2.displayAnimate();

    if (done) {
      if (mat2Greeting) {
        // Greeting scrolled once — return to idle / next message
        mat2Greeting = false;
        mat2Busy     = false;
        mat2.displayClear();
        if (uState == U_GREETING) enterUState(U_COOLDOWN);

      } else {
        // Supabase message — scroll up to 3 passes
        mat2Passes++;
        if (mat2Passes < 3) {
          // Restart same message for another pass
          mat2StartScroll(mat2CurText.c_str(), false, MSG_SPEED);
        } else {
          Serial.println("Message done (3 passes).");
          mat2TryNextMessage();
        }
      }
    }
  } else {
    // Idle: start next queued message if available (only when not greeting)
    if (!msgQ.empty() && uState == U_IDLE) {
      mat2TryNextMessage();
    }
  }

  // ── Ultrasonic state machine ──────────────────────────────────────────────
  switch (uState) {

    case U_IDLE: {
      long dist = getDistance();
      if (dist > 0 && dist < DETECT_CM) {
        // Object detected — trigger greeting
        // Interrupt any scrolling message (it will resume after cooldown)
        if (mat2Busy && !mat2Greeting) {
          // Re-queue the current message so it's not lost
          msgQ.push({ "", mat2CurText });   // empty id = already marked in DB
          mat2Passes = 0;
        }
        enterUState(U_GREETING);
      }
      break;
    }

    case U_GREETING: {
      // Safety timeout — if greeting somehow stalls, move on
      if (millis() - uStateStart > GREET_MS + 4000) {
        mat2Greeting = false;
        mat2Busy     = false;
        mat2.displayClear();
        enterUState(U_COOLDOWN);
      }
      break;
    }

    case U_COOLDOWN: {
      if (millis() - uStateStart >= COOLDOWN_MS) {
        enterUState(U_IDLE);
      }
      break;
    }
  }

  // Small yield to prevent watchdog resets on ESP32
  delay(5);
}
