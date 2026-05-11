
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
// ── NEW: Supabase message polling ──────────────────────────
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <queue>
#include <set>

// ── WiFi Credentials ──────────────────────────────────────
const char* ssid     = "marimar";
const char* password = "martin061106";

// ── NTP Settings (UTC+8 Philippine Time) ──────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.google.com", 28800, 60000);

// ── RTC (DS3231) ──────────────────────────────────────────
RTC_DS3231 rtc;
bool rtcAvailable  = false;   // true if RTC chip is found on I2C
bool rtcTimeValid  = false;   // true if RTC has a reliable time (battery OK + NTP synced)

// ── Alarm Settings ────────────────────────────────────────
const int ALARM_HOUR   = 00;
const int ALARM_MINUTE = 10;

// ── Hardware Pins ─────────────────────────────────────────
const int BUZZER_PIN = 18;

// ── Ultrasonic Pins ───────────────────────────────────────
const int TRIG_PIN             = 23;
const int ECHO_PIN             = 35;
const int DETECT_DISTANCE_CM   = 20;          // detect within 20 cm
const long U_DISPLAY_DURATION  = 10000;       // scroll "Hello" for 10 s
const long U_COOLDOWN_DURATION = 2000;        // cooldown before re-arm

// ── LCD I2C (SDA=21, SCL=22) ──────────────────────────────
// Address auto-detected at boot: 0x27 (PCF8574) or 0x3F (PCF8574A)
uint8_t lcdAddr = 0x27;
LiquidCrystal_I2C lcd(lcdAddr, 16, 2);

// ── State ─────────────────────────────────────────────────
bool alarmTriggered = false;
bool displayOn      = false;   // start false; set true after switch confirmed ON
bool clockActive    = false;

// ── Ultrasonic State Machine ──────────────────────────────
enum UltraState { U_IDLE, U_DISPLAYING, U_COOLDOWN };
UltraState    ultraState      = U_IDLE;
unsigned long ultraStateStart = 0;

// ── Row-1 content tracker ────────────────────────────────
// When NOT showing "Hello", row 1 is blank.
bool showingHello = false;

// ── Day array ─────────────────────────────────────────────
const char* DAYS[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

// ── NEW: Supabase Configuration ───────────────────────────
const char* SUPABASE_URL = "https://vzsvojwsdwgfnwajmpxp.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InZ6c3ZvandzZHdnZm53YWptcHhwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzgxMjYwMDIsImV4cCI6MjA5MzcwMjAwMn0.Ir9hHWDHkXtZXJYLop73cz1pBsgUch-6UgIThKnOqN4";
const unsigned long POLL_INTERVAL_MS = 10000; // Poll every 10 seconds
const unsigned long MSG_SCROLL_SPEED = 150;   // ms per scroll step (150 = smooth)

// ── NEW: Message Queue ────────────────────────────────────
struct QueuedMsg {
  String id;
  String content;
};
std::queue<QueuedMsg> msgQueue;       // FIFO queue of approved messages
std::set<String>  queuedIds;          // IDs already queued this session (dedup guard)
bool          msgScrolling   = false; // true while a message is being scrolled on row 1
String        currentMsgFull = "";    // Padded full string for current scroll
int           msgScrollPos   = 0;     // Current character offset in scroll string
int           msgScrollRound = 0;     // How many full passes completed (stops at 3)
unsigned long lastMsgTick    = 0;     // Timestamp of last scroll step
unsigned long lastPoll       = 0;     // Timestamp of last Supabase poll

// ── Zero-pad helper ───────────────────────────────────────
String pad2(int n) { return (n < 10 ? "0" : "") + String(n); }

// ── Get DateTime (RTC preferred, NTP fallback) ────────────
// Uses RTC only if chip found AND time is valid (year > 2020).
// Falls back to NTP epoch otherwise — works even with dead battery.
DateTime getNow() {
  if (rtcAvailable) {
    DateTime t = rtc.now();
    // RTC year 2000 means it lost power and was never set — use NTP instead
    if (t.year() > 2020) {
      rtcTimeValid = true;
      return t;
    }
  }
  // NTP fallback: getEpochTime() includes the UTC+8 offset we set (28800s)
  rtcTimeValid = false;
  unsigned long epoch = timeClient.getEpochTime();
  if (epoch > 1000000000UL) {
    return DateTime(epoch);
  }
  // NTP not ready yet — return a safe zero DateTime
  return DateTime(2024, 1, 1, 0, 0, 0);
}

// ── Sync RTC from NTP ─────────────────────────────────────
void syncRTCfromNTP() {
  // Always force-refresh the NTPClient regardless of RTC presence
  timeClient.forceUpdate();
  unsigned long epoch = timeClient.getEpochTime();
  if (epoch <= 1000000000UL) {
    Serial.println("NTP time invalid, skipping RTC sync.");
    return;
  }
  if (rtcAvailable) {
    rtc.adjust(DateTime(epoch));
    rtcTimeValid = true;
    Serial.println("RTC synced from NTP. Time: " + DateTime(epoch).timestamp());
  } else {
    // No RTC — NTPClient is the only source; log the current time
    Serial.println("NTP-only time: " + DateTime(epoch).timestamp());
  }
}

// ── Build Row 0: "12:30:45 PM  TUE" ────────
// Format: HH:MM:SS XM  DOW  (16 chars total)
String buildTimeRow(DateTime now) {
  int h = now.hour();
  int m = now.minute();
  int s = now.second();
  const char* ampm = (h < 12) ? "AM" : "PM";
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;
  char buf[17];
  // 12:30:45 PM  TUE -> exactly 16 chars
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s  %s", h12, m, s, ampm, DAYS[now.dayOfTheWeek()]);
  buf[16] = '\0';
  return String(buf);
}

// ── Update LCD Row 0 (time/date — always shown) ───────────
void updateTimeRow(DateTime now, bool force = false) {
  static int lastSec = -1;
  // Only send I2C commands if the second has changed (prevents LCD corruption from I2C flooding)
  if (now.second() != lastSec || force) {
    lcd.setCursor(0, 0);
    lcd.print(buildTimeRow(now));
    lastSec = now.second();
  }
}

// ── Clear row 1 (blank) ───────────────────────────────────
void clearRow1() {
  lcd.setCursor(0, 1);
  lcd.print("                ");  // 16 spaces
}

// ── Full clock refresh ────────────────────────────────────
void showClockDisplay(bool force = false) {
  if (!clockActive) return;
  DateTime now = getNow();
  updateTimeRow(now, force);
  // Row 1 managed by ultrasonic state machine and message scroller
}

// ── HC-SR04 distance measurement ─────────────────────────
long getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999;
  return (long)(duration * 0.034 / 2);
}

// ── Enter a new ultrasonic state ──────────────────────────
void enterUltraState(UltraState s) {
  ultraState      = s;
  ultraStateStart = millis();

  if (s == U_DISPLAYING) {
    // Initial print is handled dynamically in loop() to create scroll effect
    // Short beep only
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    showingHello = true;
    // NEW: Reset message scroll position so it restarts cleanly after Hello finishes
    msgScrollPos = 0;
    Serial.println(">> DETECTED — scrolling Hello on row 1, short beep");

  } else if (s == U_COOLDOWN) {
    clearRow1();
    showingHello = false;
    Serial.println(">> Cooldown...");

  } else { // U_IDLE
    clearRow1();
    showingHello = false;
    showClockDisplay(true); // force redraw row 0 when returning to idle
    Serial.println(">> IDLE — scanning...");
  }
}

unsigned long timeInUltraState() {
  return millis() - ultraStateStart;
}

// ── Static two-line LCD message (used during boot only) ───
void displayMessage(String row0, String row1 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(row0);
  if (row1.length() > 0) {
    lcd.setCursor(0, 1);
    lcd.print(row1);
  }
}

// ── Alarm sound sequence ──────────────────────────────────
void soundAlarm() {
  lcd.clear();
  lcd.setCursor(2, 0); lcd.print("** ALARM! **");
  lcd.setCursor(1, 1); lcd.print(" Wake up! ");

  for (int burst = 0; burst < 3; burst++) {
    for (int beep = 0; beep < 4; beep++) {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(80);
      digitalWrite(BUZZER_PIN, LOW);
      delay(60);
    }
    delay(300);
  }
  digitalWrite(BUZZER_PIN, HIGH);
  delay(400);
  digitalWrite(BUZZER_PIN, LOW);
  lcd.clear();
}

// ── NEW: Mark a message as displayed in Supabase ──────────
// Called immediately after fetching so it won't be re-fetched on next poll.
void markAsDisplayed(String id) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/messages?id=eq." + id;
  http.begin(url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  int code = http.PATCH("{\"displayed\":true}");
  if (code == 204 || code == 200) {
    Serial.println("  Marked displayed: id=" + id);
  } else {
    Serial.println("  markAsDisplayed HTTP error: " + String(code));
  }
  http.end();
}

// ── NEW: Fetch approved, not-yet-displayed messages from Supabase ──
// Adds them to msgQueue in chronological order (oldest first).
// Immediately marks each as displayed so they won't repeat on next poll.
void fetchApprovedMessages() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  // Filter: status=approved AND displayed=false, ordered oldest first
  String url = String(SUPABASE_URL)
    + "/rest/v1/messages"
    + "?status=eq.approved"
    + "&displayed=eq.false"
    + "&select=id,content"
    + "&order=created_at.asc";

  http.begin(url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    Serial.println("Poll response: " + payload);

    // ArduinoJson v6: DynamicJsonDocument  |  v7: JsonDocument
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject row : arr) {
        String msgId      = row["id"].as<String>();
        String msgContent = row["content"].as<String>();
        // Dedup guard: skip if already queued this session
        if (queuedIds.count(msgId) == 0) {
          queuedIds.insert(msgId);
          msgQueue.push({ msgId, msgContent });
          Serial.println("  Queued: [" + msgId + "] " + msgContent);
          // Mark as displayed in DB so it won't be fetched after reboot
          markAsDisplayed(msgId);
        } else {
          Serial.println("  Skipped duplicate: [" + msgId + "]");
        }
      }
    } else {
      Serial.println("JSON parse error: " + String(err.c_str()));
    }
  } else {
    Serial.println("Supabase poll HTTP error: " + String(code));
  }
  http.end();
}

// ── NEW: Advance the message scroll animation on row 1 ────
// Only called when ultraState == U_IDLE so it never fights "Hello".
// Scrolls right-to-left: message enters from the right edge of the display.
// When a message finishes, the next queued message starts automatically.
// If the queue is empty, row 1 is blank.
void tickMessageScroll() {
  unsigned long now = millis();

  // ── Start the next message if nothing is currently scrolling ──
  if (!msgScrolling) {
    if (msgQueue.empty()) return;           // Nothing to show
    QueuedMsg next = msgQueue.front();
    msgQueue.pop();
    // 16 leading spaces: message enters from right
    // 16 trailing spaces: message fully exits left before round ends
    currentMsgFull = "                " + next.content + "                ";
    msgScrollPos   = 0;
    msgScrollRound = 0;     // Reset round counter for each new message
    msgScrolling   = true;
    lastMsgTick    = now;
    Serial.println("Scrolling message: " + next.content);
    return; // Will display on next tick
  }

  // ── Guard: if we were paused a long time (e.g. HTTP fetch blocked),
  //    just re-sync the tick timer and resume from current position.
  //    Note: ultrasonic resets msgScrollPos directly in enterUltraState().
  if (now - lastMsgTick > MSG_SCROLL_SPEED * 4) {
    lastMsgTick = now;
    return;
  }

  // ── Advance one scroll step ──────────────────────────────
  if (now - lastMsgTick >= MSG_SCROLL_SPEED) {
    lastMsgTick = now;

    // Extract 16-char window, padding with spaces if near the end
    String window = "";
    for (int i = 0; i < 16; i++) {
      int idx = msgScrollPos + i;
      if (idx < (int)currentMsgFull.length()) {
        window += currentMsgFull[idx];
      } else {
        window += ' ';
      }
    }

    lcd.setCursor(0, 1);
    lcd.print(window);

    msgScrollPos++;

    // ── End of one pass: either loop or stop after 3 rounds ──
    if (msgScrollPos >= (int)currentMsgFull.length()) {
      msgScrollRound++;
      if (msgScrollRound >= 3) {
        // All 3 rounds done — clear row 1 and move to next message
        msgScrolling = false;
        msgScrollRound = 0;
        clearRow1();
        Serial.println("Message scroll complete (3 rounds).");
      } else {
        // Start another round
        msgScrollPos = 0;
        Serial.println("Round " + String(msgScrollRound + 1) + " of 3...");
      }
    }
  }
}

// ─────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────
void setup() {
  // Force buzzer OFF immediately at boot
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.begin(115200);
  delay(500);

  // I2C init
  Wire.begin(21, 22);
  delay(500);

  // ── I2C Scanner: find all devices, auto-detect LCD address ──
  Serial.println("Scanning I2C bus...");
  uint8_t foundAddr = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  I2C device found at 0x%02X\n", addr);
      if (addr == 0x27 || addr == 0x3F) foundAddr = addr;
    }
  }
  if (foundAddr != 0 && foundAddr != lcdAddr) {
    lcdAddr = foundAddr;
    lcd = LiquidCrystal_I2C(lcdAddr, 16, 2);
    Serial.printf("LCD address updated to 0x%02X\n", lcdAddr);
  } else if (foundAddr != 0) {
    Serial.printf("LCD using address 0x%02X\n", lcdAddr);
  } else {
    Serial.println("WARNING: No LCD found on I2C bus! Check wiring.");
  }

  // ── LCD robust init (HW-61 / HD44780 needs double-init) ──
  // HD44780 requires ≥150 ms after power-on before it accepts any
  // commands — without this delay it boots into a garbage state.
  delay(200);       // power-on stabilisation (critical — fixes random symbols)
  lcd.init();       // first pass — wakes the controller
  delay(50);
  lcd.init();       // second pass — ensures HD44780 is fully initialized
  delay(50);
  lcd.backlight();
  delay(10);
  lcd.clear();
  delay(20);
  displayMessage("Booting...", "Please wait");
  Serial.println("LCD init done.");


  // Pins
  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);

  // RTC
  if (rtc.begin(&Wire)) {
    rtcAvailable = true;
    Serial.println("RTC DS3231 found.");
    if (rtc.lostPower()) {
      Serial.println("RTC lost power! Will sync from NTP after WiFi connects.");
    } else {
      Serial.println("RTC running: " + String(rtc.now().timestamp()));
    }
  } else {
    rtcAvailable = false;
    Serial.println("RTC not found! Falling back to NTP only.");
    displayMessage("RTC not found!", "Using NTP only.");
    delay(1500);
  }

  // WiFi
  displayMessage("Connecting WiFi.", "Please wait...");
  Serial.print("Connecting to WiFi");

  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();
  int dot = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart < 15000)) {
    delay(500);
    Serial.print(".");
    String dots = "";
    for (int i = 0; i <= dot % 4; i++) dots += ".";
    lcd.setCursor(0, 1);
    lcd.print("Please wait" + dots + "    ");
    dot++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    displayMessage("WiFi Connected!", WiFi.localIP().toString());
    timeClient.begin();
    timeClient.forceUpdate();
    syncRTCfromNTP();
  } else {
    Serial.println("\nWiFi Timeout!");
    displayMessage("No WiFi!", rtcAvailable ? "Using RTC time." : "Offline mode.");
  }
  delay(1500);

  // Display segment test
  displayMessage("  88:88:88 OK  ", " Display Test  ");
  delay(1000);

  clockActive = true;
  lcd.clear();
  lcd.backlight();
  displayOn = true;

  enterUltraState(U_IDLE);
}

// ─────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────
void loop() {
  static unsigned long lastWifiCheck = 0;
  static unsigned long lastNtpSync   = 0;

  // ── Silent WiFi reconnect check every 30 s ──────────────
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost. Reconnecting silently...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
  }

  // ── NTP sync every 60 s (first sync fires after 10 s) ──────────
  // syncRTCfromNTP() now handles both RTC-present and NTP-only modes.
  // In NTP-only mode it just refreshes the NTPClient so getEpochTime() is current.
  unsigned long ntpInterval = (lastNtpSync == 0) ? 10000UL : 60000UL;
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpSync > ntpInterval) {
    lastNtpSync = millis();
    syncRTCfromNTP();
  }

  // ── NEW: Supabase poll every 10 s ────────────────────────
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > POLL_INTERVAL_MS) {
    lastPoll = millis();
    Serial.println("Polling Supabase...");
    fetchApprovedMessages();
  }

  // ── Current time from RTC ────────────────────────────────
  DateTime now = getNow();
  int h = now.hour();
  int m = now.minute();
  int s = now.second();

  // ── Alarm reset (outside the alarm minute) ───────────────
  if (!(h == ALARM_HOUR && m == ALARM_MINUTE)) {
    alarmTriggered = false;
  }

  // ── Trigger alarm within first 10 seconds of alarm minute ─
  if (h == ALARM_HOUR && m == ALARM_MINUTE && s < 10 && !alarmTriggered) {
    alarmTriggered = true;
    Serial.printf("ALARM! %02d:%02d\n", h, m);
    soundAlarm();
    // After alarm, re-arm ultrasonic from idle
    enterUltraState(U_IDLE);
  }

  // ── Ultrasonic state machine ─────────────────────────────
  switch (ultraState) {

    case U_IDLE: {
      long dist = getDistance();
      Serial.printf("Distance: %ld cm\n", dist);
      showClockDisplay();   // keep row 0 updated
      // NEW: Scroll approved messages on row 1 when idle
      tickMessageScroll();
      if (dist > 0 && dist < DETECT_DISTANCE_CM) {
        // Update row 0 before switching so it's current
        updateTimeRow(getNow());
        enterUltraState(U_DISPLAYING);
      }
      break;
    }

    case U_DISPLAYING: {
      // Keep row 0 (clock) current
      showClockDisplay();
      
      unsigned long elapsed = timeInUltraState();
      if (elapsed >= U_DISPLAY_DURATION) {
        enterUltraState(U_COOLDOWN);
      } else {
        // Looping Left-to-Right sliding animation
        String text = "                Hello, there!                ";
        int stepDelay = 170;        // 170ms per shift for a much slower, relaxed scroll
        int totalSteps = 30;        // slide all the way across
        int currentStep = (elapsed / stepDelay) % totalSteps;
        
        // Offset decreases from 29 down to 0 (makes text move Left-to-Right on screen)
        // Once it finishes 30 steps, currentStep resets to 0, which makes it spawn back on the left!
        int offset = 29 - currentStep;
        
        static int lastOffset = -1;
        // Only push to LCD if the marquee actually shifted (prevents I2C flooding)
        if (offset != lastOffset) {
          lcd.setCursor(0, 1);
          lcd.print(text.substring(offset, offset + 16));
          lastOffset = offset;
        }
      }
      break;
    }

    case U_COOLDOWN: {
      showClockDisplay();
      if (timeInUltraState() >= U_COOLDOWN_DURATION) {
        enterUltraState(U_IDLE);
      }
      break;
    }
  }

  delay(50);
}