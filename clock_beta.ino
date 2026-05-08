
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ── WiFi Credentials ──────────────────────────────────────
const char* ssid     = "Bicol University WiFi";
const char* password = "BUp@ssw0rd";

// ── NTP Settings (UTC+8 Philippine Time) ──────────────────
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 28800, 60000);

// ── RTC (DS3231) ──────────────────────────────────────────
RTC_DS3231 rtc;
bool rtcAvailable = false;

// ── Alarm Settings ────────────────────────────────────────
const int ALARM_HOUR   = 13;
const int ALARM_MINUTE = 17;

// ── Hardware Pins ─────────────────────────────────────────
const int SWITCH_PIN = 19;
const int BUZZER_PIN = 18;

// ── Ultrasonic Pins ───────────────────────────────────────
const int TRIG_PIN             = 23;
const int ECHO_PIN             = 35;
const int DETECT_DISTANCE_CM   = 20;          // detect within 20 cm
const long U_DISPLAY_DURATION  = 2000;        // show "Hello" for 2 s
const long U_COOLDOWN_DURATION = 3000;        // cooldown before re-arm

// ── LCD I2C (SDA=21, SCL=22) ──────────────────────────────
// NOTE: If display shows garbage, change 0x27 to 0x3F
LiquidCrystal_I2C lcd(0x27, 16, 2);

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

// ── Zero-pad helper ───────────────────────────────────────
String pad2(int n) { return (n < 10 ? "0" : "") + String(n); }

// ── Get DateTime (RTC preferred, NTP fallback) ────────────
DateTime getNow() {
  if (rtcAvailable) return rtc.now();
  return DateTime(timeClient.getEpochTime());
}

// ── Sync RTC from NTP ─────────────────────────────────────
void syncRTCfromNTP() {
  if (!rtcAvailable) return;
  unsigned long epoch = timeClient.getEpochTime();
  if (epoch > 1000000000UL) {
    rtc.adjust(DateTime(epoch));
    Serial.println("RTC synced from NTP.");
  } else {
    Serial.println("NTP time invalid, skipping RTC sync.");
  }
}

// ── Build Row 0: military format "12:30 PM   TUE" ────────
// Format: HH:MM XM   DOW  (16 chars total)
String buildTimeRow(DateTime now) {
  int h = now.hour();
  int m = now.minute();
  const char* ampm = (h < 12) ? "AM" : "PM";
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;
  char buf[17];
  snprintf(buf, sizeof(buf), "%02d:%02d %s   %s   ", h12, m, ampm, DAYS[now.dayOfTheWeek()]);
  buf[16] = '\0';
  return String(buf);
}

// ── Update LCD Row 0 (time/date — always shown) ───────────
void updateTimeRow(DateTime now) {
  lcd.setCursor(0, 0);
  lcd.print(buildTimeRow(now));
}

// ── Clear row 1 (blank) ───────────────────────────────────
void clearRow1() {
  lcd.setCursor(0, 1);
  lcd.print("                ");  // 16 spaces
}

// ── Full clock refresh ────────────────────────────────────
void showClockDisplay() {
  if (!clockActive) return;
  DateTime now = getNow();
  updateTimeRow(now);
  // Row 1 managed by ultrasonic state machine
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
    // Row 0 keeps the clock — only row 1 changes
    lcd.setCursor(0, 1);
    lcd.print("  Hello, there! ");
    // Short beep only
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    showingHello = true;
    Serial.println(">> DETECTED — showing Hello on row 1, short beep");

  } else if (s == U_COOLDOWN) {
    clearRow1();
    showingHello = false;
    Serial.println(">> Cooldown...");

  } else { // U_IDLE
    clearRow1();
    showingHello = false;
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
  lcd.setCursor(1, 1); lcd.print("Flip switch OFF!");

  for (int burst = 0; burst < 3; burst++) {
    for (int beep = 0; beep < 4; beep++) {
      if (digitalRead(SWITCH_PIN) == HIGH) {
        digitalWrite(BUZZER_PIN, LOW);
        return;
      }
      digitalWrite(BUZZER_PIN, HIGH);
      delay(80);
      digitalWrite(BUZZER_PIN, LOW);
      delay(60);
    }
    if (digitalRead(SWITCH_PIN) == HIGH) {
      digitalWrite(BUZZER_PIN, LOW);
      return;
    }
    delay(300);
  }
  if (digitalRead(SWITCH_PIN) == HIGH) {
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }
  digitalWrite(BUZZER_PIN, HIGH);
  delay(400);
  digitalWrite(BUZZER_PIN, LOW);
  lcd.clear();
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

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  displayMessage("Booting...", "Please wait");

  // Pins
  pinMode(SWITCH_PIN, INPUT_PULLUP);
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

  // Determine initial display state from switch
  if (digitalRead(SWITCH_PIN) == LOW) {
    // Switch is ON
    lcd.backlight();
    displayOn = true;
  } else {
    // Switch is OFF
    lcd.noBacklight();
    lcd.clear();
    displayOn = false;
    clockActive = false;
  }

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

  // ── NTP → RTC sync every 60 s ───────────────────────────
  // RTC keeps time between syncs; accurate even if WiFi dies.
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpSync > 60000UL) {
    lastNtpSync = millis();
    timeClient.forceUpdate();
    syncRTCfromNTP();
  }

  // ── Power switch: LOW = ON (INPUT_PULLUP, switch to GND) ─
  bool switchIsOn = (digitalRead(SWITCH_PIN) == LOW);

  if (!switchIsOn) {
    // Switch OFF → blank display, silence buzzer
    if (displayOn) {
      lcd.noBacklight();
      lcd.clear();
      displayOn   = false;
      clockActive = false;
      digitalWrite(BUZZER_PIN, LOW);
      Serial.println("Switch OFF — display off.");
    }
    delay(50);
    return;  // nothing else to do
  }

  // Switch ON → restore display if it was off
  if (!displayOn) {
    lcd.backlight();
    lcd.clear();
    displayOn   = true;
    clockActive = true;
    showingHello = false;
    enterUltraState(U_IDLE);
    Serial.println("Switch ON — display restored.");
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
      if (dist > 0 && dist < DETECT_DISTANCE_CM) {
        // Update row 0 before switching so it's current
        updateTimeRow(getNow());
        enterUltraState(U_DISPLAYING);
      }
      break;
    }

    case U_DISPLAYING: {
      // Keep row 0 (clock) current while "Hello" shows on row 1
      showClockDisplay();
      if (timeInUltraState() >= U_DISPLAY_DURATION) {
        enterUltraState(U_COOLDOWN);
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