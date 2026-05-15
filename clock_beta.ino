#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
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
#include <vector>

// ── WiFi provisioning (Library Manager: "WiFiManager" by tzapu) ───────────────
// First boot (or no saved network): join AP WIFI_AP_NAME, open captive portal, pick home Wi‑Fi.
// Change WIFI_AP_PASSWORD and OTA_UPLOAD_PASSWORD before sealing the device (min 8 chars for AP WPA).
const char* const WIFI_AP_NAME         = "ClockSetup";
const char* const WIFI_AP_PASSWORD     = "PortalChg1";  // join ClockSetup with this password; change it
const unsigned long WIFI_PORTAL_SEC    = 300;          // portal max time if STA connect fails (0 = never)

// OTA: same password in firmware and in Arduino IDE when uploading over network
const char* const OTA_UPLOAD_PASSWORD  = "clock-ota-change-me";


// ── NTP (UTC+8 Philippine Time) ───────────────────────────────────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 28800, 60000);

// ── RTC DS3231 ────────────────────────────────────────────────────────────────
RTC_DS3231 rtc;
bool rtcAvailable = false;

// ── Alarm ─────────────────────────────────────────────────────────────────────
const int ALARM_HOUR   = 19;
const int ALARM_MINUTE = 11;
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

// Module 1 — Info (time / date / weather)
// We will use SOFTWARE SPI (bit-banging) for both displays. 
// Hardware SPI on the ESP32 is often too fast for 5V MAX7219s without a level shifter,
// causing only the first block to light up!
#define MAT1_DIN  23
#define MAT1_CLK  18
#define MAT1_CS   5
MD_Parola mat1(HW_TYPE, MAT1_DIN, MAT1_CLK, MAT1_CS, NUM_DEVICES);

// Module 2 — Text/message — Software SPI
#define MAT2_DIN  19
#define MAT2_CLK  17
#define MAT2_CS   16
MD_Parola mat2(HW_TYPE, MAT2_DIN, MAT2_CLK, MAT2_CS, NUM_DEVICES);

// ── Display brightness (0 dim … 15 bright) ────────────────────────────────────
// Lowered to 0 for testing. High brightness can cause voltage drops at the end of the chain!
const uint8_t BRIGHTNESS = 0;

// ── Scroll speed: lower number = FASTER scroll (ms per step internally) ───────
const uint8_t INFO_SPEED  = 50;   // mat1 scroll speed
const uint8_t MSG_SPEED   = 45;   // mat2 message scroll speed
const uint8_t GREET_SPEED = 55;   // mat2 greeting scroll speed

// ── Alarm & buzzer timing constants ──────────────────────────────────────────
constexpr uint16_t GREET_BUZZ_MS   = 120;
constexpr uint16_t ALARM_BEEP_ON   =  80;
constexpr uint16_t ALARM_BEEP_OFF  =  60;
constexpr uint16_t ALARM_BURST_GAP = 300;
constexpr uint16_t ALARM_LONG_BUZZ = 500;
constexpr uint8_t  ALARM_SPEED     =  35;

// ── Static time display refresh ───────────────────────────────────────────────
int lastMinDisplayed = -1;   // tracks the last minute rendered (renamed from lastSecDisplayed)

// ── Supabase ──────────────────────────────────────────────────────────────────
const char* SB_URL = "https://vzsvojwsdwgfnwajmpxp.supabase.co";
const char* SB_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
  ".eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InZ6c3ZvandzZHdnZm53YWptcHhwIiwi"
  "cm9sZSI6ImFub24iLCJpYXQiOjE3NzgxMjYwMDIsImV4cCI6MjA5MzcwMjAwMn0"
  ".Ir9hHWDHkXtZXJYLop73cz1pBsgUch-6UgIThKnOqN4";

const unsigned long POLL_MS = 10000UL;
unsigned long lastPoll      = 0;

// ── WiFi & Network timing constants (magic numbers converted) ────────────────
const unsigned long WIFI_CHECK_INTERVAL = 30000UL;      // WiFi check every 30s
const unsigned long WIFI_TIMEOUT = 15000UL;             // WiFi connection timeout
const unsigned long WIFI_RECONNECT_INITIAL = 5000UL;    // Start with 5s backoff
const unsigned long WIFI_RECONNECT_MAX = 120000UL;      // Max 2min backoff
const unsigned long NTP_SYNC_FAST = 10000UL;            // Fast sync when not synced (10s)
const unsigned long NTP_SYNC_NORMAL = 3600000UL;        // Normal sync every 1 hour (was 60s)
const unsigned long BOOT_TIMEOUT = 400;                 // Boot delay
const unsigned long I2C_INIT_DELAY = 300;               // I2C initialization delay
const unsigned int SERIAL_BAUD = 115200;                // Serial baud rate
const unsigned long SEMAPHORE_TIMEOUT = 500UL;          // Semaphore timeout (500ms)

// ── Safety constants for message handling & memory protection ─────────────────
const size_t MAX_MSG_LENGTH = 150;                        // Max message chars (buffer overflow protection)
const size_t MAX_SEEN_IDS = 500;                          // Max entries in seenIds set
const unsigned long DATE_TOGGLE_MS = 15000UL;             // Time-based date toggle (was state-based)
const uint16_t I2C_TIMEOUT_MS = 100;                      // I2C operation timeout

// ── State variables for robustness ───────────────────────────────────────────
bool mat2GreetingCancelled = false;                       // Flag: greeting interrupted by message
unsigned long lastDateToggleTime = 0;                     // Timestamp of last date toggle
static DateTime lastGoodTime = DateTime(2024, 1, 1, 0, 0, 0);  // Fallback time if all sources fail

// ── Error Logging Macro ────────────────────────────────────────────────────────
#define LOG_ERROR(msg) Serial.print("[ERROR] "); Serial.println(msg);
#define LOG_WARN(msg)  Serial.print("[WARN]  "); Serial.println(msg);
#define LOG_INFO(msg)  Serial.print("[INFO]  "); Serial.println(msg);

// ── WiFi Backoff State ─────────────────────────────────────────────────────────
unsigned long wifiReconnectBackoff = WIFI_RECONNECT_INITIAL;
unsigned long lastWifiAttempt = 0;
bool wifiConnected = false;

// ── Message queue for mat2 ────────────────────────────────────────────────────
struct Msg { String id; String text; };
std::queue<Msg> msgQ;
std::set<String> seenIds;

SemaphoreHandle_t msgMutex;
SemaphoreHandle_t i2cMutex;

bool   mat2Busy     = false;
bool   mat2Greeting = false;
bool   mat2IsDate   = false;
bool   mat2DateToggle = false; // toggles between "05/11/26" and "MON"
int    mat2Passes   = 0;       // count scroll passes for the current message
String mat2CurText  = "";
String mat2SavedMsgId = "";    // saved message ID when alarm interrupts
String mat2SavedMsgText = "";  // saved message text when alarm interrupts
bool   mat2MsgInterruptedByAlarm = false;  // flag: message was interrupted by alarm

// Static buffer for Parola (must persist while animating)
static char mat2Buf[128];
static char mat1Buf[32];

// ── Lookup tables ──────────────────────────────────────────────────────────────
const char* DAYS[]   = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
const char* MONTHS[] = { "JAN","FEB","MAR","APR","MAY","JUN",
                          "JUL","AUG","SEP","OCT","NOV","DEC" };



// ═════════════════════════════════════════════════════════════════════════════
// RTC / NTP helpers
// ═════════════════════════════════════════════════════════════════════════════
DateTime getNow() {
  if (rtcAvailable) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE) {
      DateTime t = rtc.now();
      xSemaphoreGive(i2cMutex);
      if (t.year() > 2020) {
        lastGoodTime = t;  // Update fallback time
        return t;
      }
    } else {
      LOG_WARN("RTC read timeout");
    }
  }
  unsigned long ep = timeClient.getEpochTime();
  if (ep > 1000000000UL) {
    DateTime dt(ep);
    lastGoodTime = dt;  // Update fallback time
    return dt;
  }
  // Both RTC and NTP failed: return last-known-good time
  return lastGoodTime;
}

void syncRTC() {
  timeClient.forceUpdate();
  unsigned long ep = timeClient.getEpochTime();
  if (ep <= 1000000000UL) { Serial.println("NTP invalid."); return; }
  if (rtcAvailable) {
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE) {
      rtc.adjust(DateTime(ep));
      xSemaphoreGive(i2cMutex);
    } else {
      LOG_WARN("RTC sync timeout");
    }
  }
  Serial.println("Time synced: " + DateTime(ep).timestamp());
}

// ═════════════════════════════════════════════════════════════════════════════
// String builders for mat1
// ═════════════════════════════════════════════════════════════════════════════
void buildTimeStr(DateTime& t, char* buf, size_t len) {
  // Format as HH:MM military time (24-hour)
  snprintf(buf, len, "%02d:%02d", t.hour(), t.minute());
}



// ═════════════════════════════════════════════════════════════════════════════
// Supabase helpers
// ═════════════════════════════════════════════════════════════════════════════
void markDisplayed(const String& id) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(5000);  // 5-second timeout prevents hanging
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
  http.setTimeout(5000);  // 5-second timeout prevents hanging
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
        
        // Validate message length to prevent buffer overflow
        if (txt.length() > MAX_MSG_LENGTH) {
          txt = txt.substring(0, MAX_MSG_LENGTH);
          LOG_WARN("Message truncated to " + String(MAX_MSG_LENGTH) + " chars");
        }
        
        if (!seenIds.count(id)) {
          seenIds.insert(id);
          
          // Cleanup seenIds if it grows too large (memory leak prevention)
          if (seenIds.size() > MAX_SEEN_IDS) {
            Serial.println("[WARN] seenIds at " + String(seenIds.size()) + " entries, clearing oldest 50%");
            // Create temporary set with newest half of IDs
            std::set<String> newSeenIds;
            int count = 0;
            for (auto it = seenIds.rbegin(); it != seenIds.rend() && count < MAX_SEEN_IDS / 2; ++it, ++count) {
              newSeenIds.insert(*it);
            }
            seenIds = newSeenIds;
          }
          
          if (xSemaphoreTake(msgMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE) {
            msgQ.push({ id, txt });
            xSemaphoreGive(msgMutex);
            Serial.println("  Queued [" + id + "]: " + txt);
            markDisplayed(id);
          } else {
            LOG_WARN("Mutex timeout queueing message");
          }
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
  // Validate text length to prevent buffer overflow
  if (!text || strlen(text) == 0) {
    LOG_WARN("mat2StartScroll: empty text");
    return;
  }
  
  strncpy(mat2Buf, text, sizeof(mat2Buf) - 1);
  mat2Buf[sizeof(mat2Buf) - 1] = '\0';
  mat2.displayClear();
  // PA_SCROLL_LEFT = right-to-left marquee direction
  mat2.displayScroll(mat2Buf, PA_LEFT, PA_SCROLL_LEFT, speed);
  mat2Busy     = true;
  mat2Greeting = isGreeting;
  mat2IsDate   = false;
  Serial.print("Mat2 scroll: ");
  Serial.println(text);
}

// ═════════════════════════════════════════════════════════════════════════════
// Mat2 — try to start the next message from queue
// ═════════════════════════════════════════════════════════════════════════════
void mat2TryNextMessage() {
  if (xSemaphoreTake(msgMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) != pdTRUE) {
    LOG_WARN("Mutex timeout in mat2TryNextMessage");
    return;
  }
  
  // If alarm is running, hold all queued messages until alarm finishes
  if (alarmTriggered) {
    xSemaphoreGive(msgMutex);
    return;
  }
  
  if (msgQ.empty()) {
    xSemaphoreGive(msgMutex);
    mat2Busy = false;
    mat2IsDate = false;
    mat2.displayClear();
    return;
  }
  Msg next = msgQ.front();
  msgQ.pop();
  xSemaphoreGive(msgMutex);

  mat2CurText = next.text;
  mat2Passes  = 0;
  mat2IsDate  = false;
  mat2StartScroll(mat2CurText.c_str(), false, MSG_SPEED);
}

// ═════════════════════════════════════════════════════════════════════════════
// Mat1 — Static Time Display  (now receives pre-fetched DateTime)
// ═════════════════════════════════════════════════════════════════════════════
void mat1Tick(const DateTime& now) {
  int currentMinute = now.minute();

  // displayAnimate() MUST be called every loop.
  // It returns true when the current "display" cycle completes.
  // For PA_NO_EFFECT this happens almost immediately.
  // We re-write the text every time it completes (instead of calling
  // displayReset which briefly blanks the panel and reveals stuck pixels).
  bool done = mat1.displayAnimate();

  // Detect time jump (e.g., after NTP sync) — force refresh if minute changed by >1
  if (abs(currentMinute - lastMinDisplayed) > 1) {
    lastMinDisplayed = -1;  // Force refresh
  }

  if (done || currentMinute != lastMinDisplayed) {
    lastMinDisplayed = currentMinute;
    // cast away const — buildTimeStr only reads the DateTime
    buildTimeStr(const_cast<DateTime&>(now), mat1Buf, sizeof(mat1Buf));
    // Long pause (60 s) keeps the text static without constantly re-rendering
    mat1.displayText(mat1Buf, PA_CENTER, 0, 60000, PA_NO_EFFECT, PA_NO_EFFECT);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Ultrasonic distance (cm)
// ═════════════════════════════════════════════════════════════════════════════
long getDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000);  // 30ms covers ~500cm (increased from 6ms to improve range)
  return dur ? (long)(dur * 0.034f / 2.0f) : 999L;
}

// ═════════════════════════════════════════════════════════════════════════════
// Enter ultrasonic state
// ═════════════════════════════════════════════════════════════════════════════
void enterUState(UltraState s) {
  uState      = s;
  uStateStart = millis();

  if (s == U_GREETING) {
    digitalWrite(BUZZER_PIN, HIGH); delay(GREET_BUZZ_MS); digitalWrite(BUZZER_PIN, LOW);
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
// I2C timeout wrapper (prevents infinite hangs on stuck bus)
// ═════════════════════════════════════════════════════════════════════════════
bool i2cReadByte(uint8_t addr, uint8_t& value, uint16_t timeout_ms = 100) {
  unsigned long start = millis();
  Wire.beginTransmission(addr);
  while (Wire.endTransmission() == 4) {  // 4 = other error / unknown
    if (millis() - start > timeout_ms) {
      LOG_ERROR("I2C timeout for device 0x" + String(addr, HEX));
      return false;
    }
    delay(1);
  }
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// OTA (Arduino IDE: same LAN → Tools → Port → clock-beta at …)
// ═════════════════════════════════════════════════════════════════════════════
void beginArduinoOTA() {
  ArduinoOTA.setHostname("clock-beta");
  if (OTA_UPLOAD_PASSWORD != nullptr && OTA_UPLOAD_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(OTA_UPLOAD_PASSWORD);
  }
  ArduinoOTA
    .onStart([]() {
      Serial.println(String("[OTA] start ") +
                     ((ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem"));
    })
    .onEnd([]() { Serial.println("[OTA] end"); })
    .onProgress([](unsigned int progress, unsigned int total) {
      if (total == 0) return;
      Serial.printf("[OTA] %u%%\r", (unsigned)((progress * 100) / total));
    })
    .onError([](ota_error_t error) {
      Serial.printf("[OTA] error %u\n", (unsigned)error);
    });
  ArduinoOTA.begin();
  if (OTA_UPLOAD_PASSWORD != nullptr && OTA_UPLOAD_PASSWORD[0] != '\0') {
    Serial.println("[OTA] password required for upload");
  }
  Serial.println("[OTA] listening — use IDE network port \"clock-beta\"");
}

// ═════════════════════════════════════════════════════════════════════════════
// Network Task (Core 0)
// ═════════════════════════════════════════════════════════════════════════════
void networkTask(void *pvParameters) {
  unsigned long lastWifiCheck = 0;
  unsigned long lastNtpSync   = 0;
  unsigned long lastPoll      = 0;
  bool          ntpSynced     = true;

  for (;;) {
    bool wifiOK = (WiFi.status() == WL_CONNECTED);
    wifiConnected = wifiOK;  // Update global state (FIX: was never updated)

    if (millis() - lastWifiCheck > 30000) {
      lastWifiCheck = millis();
      if (!wifiOK) {
        Serial.println("WiFi lost. Reconnecting...");
        WiFi.disconnect(false);
        delay(200);
        WiFi.begin();  // uses credentials saved by WiFiManager
      }
    }

    if (wifiOK) {
      ArduinoOTA.handle();
      unsigned long ntpInt = ntpSynced ? 60000UL : 10000UL;
      if (millis() - lastNtpSync > ntpInt) {
        lastNtpSync = millis();
        ntpSynced   = true;
        syncRTC();
      }

      if (millis() - lastPoll > POLL_MS) {
        lastPoll = millis();
        pollSupabase();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Alarm sequence (blinking time on row 1, centered ALARM on row 2)
// ═════════════════════════════════════════════════════════════════════════════
void soundAlarm(const DateTime& now) {
  buildTimeStr(const_cast<DateTime&>(now), mat1Buf, sizeof(mat1Buf));
  static char alarmBuf2[] = "ALARM";

  mat1.displayClear();
  mat2.displayClear();
  
  // Set mat2 alignment to center for alarm message
  mat2.setTextAlignment(PA_CENTER);
  mat2.displayText(alarmBuf2, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);

  for (int burst = 0; burst < 3; burst++) {
    for (int beep = 0; beep < 4; beep++) {
      // Blink time on mat1 (row 1) — display ON
      mat1.setTextAlignment(PA_CENTER);
      mat1.displayText(mat1Buf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      mat1.displayAnimate(); mat2.displayAnimate();
      digitalWrite(BUZZER_PIN, HIGH); delay(ALARM_BEEP_ON);  yield();
      
      // Blink time OFF — clear and animate
      mat1.displayClear();
      mat1.displayAnimate();  // Execute the clear
      digitalWrite(BUZZER_PIN, LOW);  delay(ALARM_BEEP_OFF); yield();
      mat2.displayAnimate();  // Keep mat2 animated
    }
    // Gap between bursts
    delay(ALARM_BURST_GAP); yield();
  }
  
  // Final long buzz — show time
  mat1.displayText(mat1Buf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  mat1.displayAnimate(); mat2.displayAnimate();
  digitalWrite(BUZZER_PIN, HIGH); delay(ALARM_LONG_BUZZ); yield();
  digitalWrite(BUZZER_PIN, LOW);
  
  // Reset alignments and clear displays
  mat1.setTextAlignment(PA_LEFT);
  mat2.setTextAlignment(PA_LEFT);
  mat1.displayClear(); 
  mat2.displayClear();
  mat1.displayAnimate();
  mat2.displayAnimate();
}

// ═════════════════════════════════════════════════════════════════════════════
// Boot splash — scroll a string on the given matrix and wait for it to finish
// ═════════════════════════════════════════════════════════════════════════════
void bootSplash(MD_Parola& m, const char* text, uint8_t speed = 50) {
  char buf[128];   // local buffer — NOT static, so mat1 and mat2 never share it
  strncpy(buf, text, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
  m.displayClear();
  m.displayScroll(buf, PA_LEFT, PA_SCROLL_LEFT, speed);
  while (!m.displayAnimate()) { /* spin */ }
  m.displayClear();
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  // ── PREVENT BOOT GARBAGE ──────────────────────────────────────────────────
  // The MAX7219 chips can pick up noise while the ESP32 is booting up.
  // By immediately pulling the CS pins HIGH, we force them to ignore noise.
  pinMode(MAT1_CS, OUTPUT); digitalWrite(MAT1_CS, HIGH);
  pinMode(MAT2_CS, OUTPUT); digitalWrite(MAT2_CS, HIGH);

  // Buzzer OFF immediately
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== DUAL NH1088AS MATRIX BOOT ===");

  msgMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();

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
  Serial.println("Initializing I2C...");
  Wire.begin(21, 22);
  delay(I2C_INIT_DELAY);
  
  Serial.println("Scanning I2C bus...");
  // Use I2C timeout wrapper to detect RTC before attempting to initialize (prevents hangs)
  uint8_t dummy = 0;
  if (i2cReadByte(0x68, dummy, I2C_TIMEOUT_MS)) {
    Serial.println("RTC detected on I2C bus");
    if (rtc.begin(&Wire)) {
      rtcAvailable = true;
      DateTime rtcTime = rtc.now();
      Serial.println("RTC initialized: " + rtcTime.timestamp());
      
      char rtcDisplay[32];
      snprintf(rtcDisplay, sizeof(rtcDisplay), " %02d:%02d:%02d ", rtcTime.hour(), rtcTime.minute(), rtcTime.second());
      bootSplash(mat1, rtcDisplay);
      
      Serial.println("RTC time: " + String(rtcTime.hour()) + ":" + String(rtcTime.minute()) + ":" + String(rtcTime.second()));
      if (rtc.lostPower()) {
        Serial.println("RTC lost power — will sync from NTP.");
        bootSplash(mat1, " RTC Power Lost ");
      }
    } else {
      Serial.println("RTC begin() failed despite I2C presence");
      rtcAvailable = false;
      bootSplash(mat1, " RTC Init Error ");
    }
  } else {
    Serial.println("RTC not detected on I2C — NTP only");
    bootSplash(mat1, " No RTC  NTP only ");
  }

  // ── WiFi (WiFiManager: portal AP "ClockSetup" if needed) ──────────────────
  bootSplash(mat1, " Connecting WiFi... ");

  WiFi.mode(WIFI_STA);
  // do not use disconnect(true) here — it wipes saved STA credentials on ESP32
  delay(100);

  WiFiManager wm;
  wm.setConnectTimeout(WIFI_TIMEOUT / 1000);       // seconds per STA connect attempt
  wm.setConfigPortalTimeout(WIFI_PORTAL_SEC);     // seconds; then give up and boot offline

  bool gotWifi = false;
  if (WIFI_AP_PASSWORD != nullptr && WIFI_AP_PASSWORD[0] != '\0') {
    gotWifi = wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASSWORD);
  } else {
    gotWifi = wm.autoConnect(WIFI_AP_NAME);
  }

  if (gotWifi && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    int rssi = WiFi.RSSI();
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString() + " (RSSI: " + String(rssi) + "dBm)");

    bootSplash(mat1, " WiFi OK ");

    char signalMsg[32];
    if (rssi > -50) {
      snprintf(signalMsg, sizeof(signalMsg), " Signal: Excellent ");
    } else if (rssi > -60) {
      snprintf(signalMsg, sizeof(signalMsg), " Signal: Good ");
    } else if (rssi > -70) {
      snprintf(signalMsg, sizeof(signalMsg), " Signal: Fair ");
    } else {
      snprintf(signalMsg, sizeof(signalMsg), " Signal: Weak ");
    }
    bootSplash(mat2, signalMsg);
    Serial.println("WiFi Signal: " + String(rssi) + " dBm");

    beginArduinoOTA();

    timeClient.begin();
    Serial.println("Starting NTP sync...");
    bootSplash(mat1, " Syncing NTP... ");
    syncRTC();
    bootSplash(mat2, " NTP OK  Ready! ");
  } else {
    Serial.println("\nWiFi timeout or portal closed without connection.");
    bootSplash(mat1, " No WiFi!  Offline ");
    bootSplash(mat2, " NTP Only Mode ");
  }

  // ── Ready ─────────────────────────────────────────────────────────────────
  mat1.displayClear();
  mat2.displayClear();

  // Prime mat1 with the current time (static)
  {
    DateTime now = getNow();
    buildTimeStr(now, mat1Buf, sizeof(mat1Buf));
    mat1.displayText(mat1Buf, PA_CENTER, 0, 0, PA_NO_EFFECT, PA_NO_EFFECT);
    lastMinDisplayed = now.minute();
  }

  enterUState(U_IDLE);

  // Start the background network task on Core 0
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);

  Serial.println("Setup complete. Running...");
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  DateTime now = getNow();

  // ── Alarm check ───────────────────────────────────────────────────────────
  bool isAlarmTime = (now.hour() == ALARM_HOUR && now.minute() == ALARM_MINUTE);
  if (!isAlarmTime) alarmTriggered = false;
  if (isAlarmTime && now.second() < 10 && !alarmTriggered) {
    alarmTriggered = true;
    
    // Save current message if one is being displayed (not greeting, not date)
    if (mat2Busy && !mat2Greeting && !mat2IsDate && mat2CurText.length() > 0) {
      mat2SavedMsgText = mat2CurText;
      mat2MsgInterruptedByAlarm = true;
      Serial.println("Message interrupted by alarm: " + mat2SavedMsgText);
    } else {
      mat2MsgInterruptedByAlarm = false;
    }
    
    // Acquire mutex before forcing mat2 clear (prevents race with networkTask)
    if (xSemaphoreTake(msgMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE) {
      mat2Busy = false;
      mat2Greeting = false;
      mat2IsDate = false;
      mat2GreetingCancelled = false;
      mat2.displayClear();
      xSemaphoreGive(msgMutex);
    }
    
    soundAlarm(now);
    lastMinDisplayed = -1;
    enterUState(U_IDLE);
    
    // After alarm finishes, restore interrupted message to front of queue
    if (mat2MsgInterruptedByAlarm) {
      if (xSemaphoreTake(msgMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE) {
        // Re-queue the interrupted message at the front
        Msg interruptedMsg;
        interruptedMsg.text = mat2SavedMsgText;
        interruptedMsg.id = "restored_after_alarm";
        
        // Create a new queue with the restored message at the front
        std::queue<Msg> tempQ;
        tempQ.push(interruptedMsg);
        while (!msgQ.empty()) {
          tempQ.push(msgQ.front());
          msgQ.pop();
        }
        msgQ = tempQ;
        
        xSemaphoreGive(msgMutex);
        Serial.println("Restored message to queue after alarm");
      }
      mat2MsgInterruptedByAlarm = false;
    }
    
    alarmTriggered = false;
  }

  mat1Tick(now);

  bool hasMessages = false;
  if (xSemaphoreTake(msgMutex, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE) {
    hasMessages = !msgQ.empty();
    xSemaphoreGive(msgMutex);
  }

  bool showingMessage = (mat2Busy && !mat2IsDate && !mat2Greeting);
  bool disableUltrasonic = (hasMessages || showingMessage);

  // ── Mat2: message / greeting / date ───────────────────────────────────────
  if (mat2Busy) {
    bool done = mat2.displayAnimate();

    if (done) {
      if (mat2Greeting) {
        mat2Greeting = false;
        mat2Busy     = false;
        mat2GreetingCancelled = false;
        mat2.displayClear();
        if (uState == U_GREETING) enterUState(U_COOLDOWN);
        
        if (hasMessages) mat2TryNextMessage();
      } else if (mat2IsDate) {
        mat2IsDate = false;
        mat2Busy   = false;
        mat2.displayClear();
        // Only toggle if enough time has passed (prevents stuck toggle state)
        if (millis() - lastDateToggleTime > DATE_TOGGLE_MS) {
          mat2DateToggle = !mat2DateToggle;
          lastDateToggleTime = millis();
        }

        if (hasMessages) mat2TryNextMessage();
      } else {
        mat2Passes++;
        if (mat2Passes < 3) {
          mat2StartScroll(mat2CurText.c_str(), false, MSG_SPEED);
        } else {
          Serial.println("Message done (3 passes).");
          mat2TryNextMessage();
        }
      }
    } else {
      // Message interrupted by greeting cancellation or new message arrival
      if (hasMessages && (mat2IsDate || (mat2Greeting && mat2GreetingCancelled))) {
         mat2Greeting = false;
         mat2IsDate = false;
         mat2Busy = false;
         mat2GreetingCancelled = false;
         mat2.displayClear();
         if (uState == U_GREETING) enterUState(U_IDLE);
         mat2TryNextMessage();
      }
    }
  } else {
    if (uState == U_IDLE) {
      if (hasMessages) {
        mat2TryNextMessage();
      } else {
        // Time-based date toggle prevents stuck states
        bool shouldShowDate = !mat2DateToggle || (millis() - lastDateToggleTime < DATE_TOGGLE_MS / 2);
        
        if (shouldShowDate) {
          snprintf(mat2Buf, sizeof(mat2Buf), "%02d/%02d/%04d",
                   now.month(), now.day(), now.year());
          mat2.displayClear();
          mat2.displayScroll(mat2Buf, PA_LEFT, PA_SCROLL_LEFT, INFO_SPEED);
        } else {
          snprintf(mat2Buf, sizeof(mat2Buf), "%s", DAYS[now.dayOfTheWeek()]);
          mat2.displayClear();
          mat2.displayText(mat2Buf, PA_CENTER, 0, 3000, PA_NO_EFFECT, PA_NO_EFFECT);
        }
        
        mat2Busy   = true;
        mat2IsDate = true;
      }
    }
  }

  // ── Ultrasonic state machine ──────────────────────────────────────────────
  if (!disableUltrasonic) {
    switch (uState) {
      case U_IDLE: {
        long dist = getDistance();
        if (dist > 0 && dist < DETECT_CM) {
          enterUState(U_GREETING);
        }
        break;
      }
      case U_GREETING: {
        if (millis() - uStateStart > GREET_MS + 4000) {
          mat2GreetingCancelled = true;  // Mark greeting as done
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
  } else {
    if (uState != U_IDLE) {
        enterUState(U_IDLE);
    }
  }

  delay(5);
}