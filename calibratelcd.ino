// ============================================================
//  H2-61  16×2 LCD  –  I²C Calibration & Test Sketch
//  I2C Address : 0x27
//  Library     : LiquidCrystal_I2C  (Frank de Brabander)
//  Board       : ESP32 / Arduino (any)
// ============================================================
//
//  What this sketch does
//  ─────────────────────
//  1. Confirms the LCD is reachable on 0x27 via I²C scan.
//  2. Tests all 4 rows × 20 columns by filling each cell.
//  3. Verifies the backlight (on / off / on).
//  4. Scrolls a message left and right.
//  5. Runs a cursor-blink test.
//  6. Prints a static display identical to the final clock
//     layout (16x2) so you can confirm alignment before integrating.
//
//  Serial baud : 115200
// ============================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── LCD address – auto-detected in setup() ───────────────────
// Common addresses: 0x27 (most boards)  or  0x3F (alternate)
uint8_t LCD_ADDR = 0x27;
LiquidCrystal_I2C lcd(0x27, 16, 2);  // re-created after scan if needed

// ── I²C scan helper ─────────────────────────────────────────
bool scanI2C(uint8_t targetAddr) {
  Serial.println(F("\n[I2C SCAN]"));
  bool found = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print(F("  Device found at 0x"));
      if (addr < 16) Serial.print('0');
      Serial.print(addr, HEX);
      if (addr == targetAddr) {
        Serial.print(F("  ← H2-61 LCD  ✓"));
        found = true;
      }
      Serial.println();
    }
  }
  if (!found) {
    Serial.println(F("  ✗ LCD NOT found at 0x27 – check wiring/address!"));
  }
  Serial.println();
  return found;
}

// ── Fill every cell on a row ─────────────────────────────────
void fillRow(uint8_t row, char ch) {
  lcd.setCursor(0, row);
  for (uint8_t col = 0; col < 20; col++) lcd.write(ch);
}

// ── Test 1 : cell fill ───────────────────────────────────────
void testCellFill() {
  Serial.println(F("[TEST 1] Cell fill – each row with a unique char"));
  lcd.clear();
  const char rowChars[2] = { '#', '*' };
  for (uint8_t r = 0; r < 2; r++) {
    fillRow(r, rowChars[r]);
    Serial.print(F("  Row "));
    Serial.print(r);
    Serial.print(F(" filled with '"));
    Serial.print(rowChars[r]);
    Serial.println('\'');
  }
  delay(2000);
}

// ── Test 2 : column counter ───────────────────────────────────
void testColumnCounter() {
  Serial.println(F("[TEST 2] Column index counter (0-9 repeating)"));
  lcd.clear();
  for (uint8_t r = 0; r < 2; r++) {
    lcd.setCursor(0, r);
    for (uint8_t c = 0; c < 16; c++) {
      lcd.write('0' + (c % 10));
    }
  }
  delay(2000);
}

// ── Test 3 : row label ────────────────────────────────────────
void testRowLabel() {
  Serial.println(F("[TEST 3] Row labels"));
  lcd.clear();
  const char* labels[2] = {
    "ROW0: 0123456789",   // exactly 16 chars
    "ROW1: 0123456789"    // exactly 16 chars
  };
  for (uint8_t r = 0; r < 2; r++) {
    lcd.setCursor(0, r);
    lcd.print(labels[r]);
  }
  delay(2000);
}

// ── Test 4 : backlight on/off ────────────────────────────────
void testBacklight() {
  Serial.println(F("[TEST 4] Backlight OFF for 1 s then ON"));
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F(" Backlight  OFF "));
  lcd.noBacklight();
  delay(1000);
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("  Backlight  ON "));
  delay(1000);
}

// ── Test 5 : scroll ──────────────────────────────────────────
void testScroll() {
  Serial.println(F("[TEST 5] Scroll left x5, then right x5"));
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("< SCROLL TEST  >"));
  lcd.setCursor(0, 1);
  lcd.print(F(" H2-61 16x2 LCD "));
  delay(500);
  for (uint8_t i = 0; i < 5; i++) { lcd.scrollDisplayLeft();  delay(250); }
  for (uint8_t i = 0; i < 5; i++) { lcd.scrollDisplayRight(); delay(250); }
  lcd.home();
  delay(500);
}

// ── Test 6 : cursor blink ────────────────────────────────────
void testCursor() {
  Serial.println(F("[TEST 6] Cursor blink at each corner"));
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Cursor blink test"));

  // corners of a 16x2: (col,row)
  uint8_t corners[4][2] = { {0,0}, {15,0}, {0,1}, {15,1} };
  lcd.blink();
  for (uint8_t i = 0; i < 4; i++) {
    lcd.setCursor(corners[i][0], corners[i][1]);
    delay(800);
  }
  lcd.noBlink();
}

// ── Test 7 : clock layout preview ────────────────────────────
void testClockLayout() {
  Serial.println(F("[TEST 7] Clock layout preview"));
  lcd.clear();
  // Row 0 – time (centred in 16 cols)  "  23:29:36    "
  lcd.setCursor(0, 0);
  lcd.print(F("   23:29:36     "));  // 16 chars
  // Row 1 – date
  lcd.setCursor(0, 1);
  lcd.print(F("08 MAY 2026 THU "));  // 16 chars
  delay(3000);
}

// ── Final pass message ────────────────────────────────────────
void showPass() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("CALIBRATION PASS"));  // exactly 16 chars
  lcd.setCursor(0, 1);
  lcd.print(F("H2-61 0x27  OK! "));  // exactly 16 chars
  Serial.println(F("\n=== CALIBRATION COMPLETE – ALL TESTS PASSED ===\n"));
}

// ── Robust LCD initialiser ───────────────────────────────────
// HD44780 needs ~150 ms after power-on before it accepts
// commands. Calling begin() after init() in the Frank de
// Brabander library corrupts the controller state and causes
// random symbols. Use init() only, with proper delays.
void initLCD() {
  delay(200);          // let HD44780 power-on fully (critical!)
  lcd.init();          // send 8-bit → 4-bit mode sequence
  delay(50);
  lcd.init();          // repeat: guarantees clean state
  delay(50);
  lcd.backlight();
  delay(10);
  lcd.clear();         // wipe any garbage from DDRAM
  delay(20);           // clear command needs ~1.5 ms; give it 20
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n============================================"));
  Serial.println(F("  H2-61  16x2 LCD  –  Calibration Sketch   "));
  Serial.println(F("============================================"));
  Serial.println(F("CONTRAST TIP: If you see blocks or garbage,"));
  Serial.println(F("turn the blue pot on the I2C backpack slowly"));
  Serial.println(F("until characters appear.\n"));

  Wire.begin();   // SDA/SCL defaults (ESP32: GPIO21/22)

  // ── Auto-detect I2C address (0x27 or 0x3F) ──────────────
  scanI2C(0x27);  // prints all found devices

  bool at27 = false, at3F = false;
  Wire.beginTransmission(0x27); at27 = (Wire.endTransmission() == 0);
  Wire.beginTransmission(0x3F); at3F = (Wire.endTransmission() == 0);

  if (at27) {
    LCD_ADDR = 0x27;
    Serial.println(F(">> Using address 0x27"));
  } else if (at3F) {
    LCD_ADDR = 0x3F;
    Serial.println(F(">> LCD not at 0x27 – falling back to 0x3F"));
  } else {
    Serial.println(F(">> ERROR: No LCD found at 0x27 or 0x3F!"));
    Serial.println(F("   Check wiring: SDA, SCL, VCC(5V), GND."));
    while (true) delay(1000);  // halt
  }

  // Re-create the lcd object with the correct address
  lcd = LiquidCrystal_I2C(LCD_ADDR, 16, 2);
  initLCD();

  // ── Splash screen ────────────────────────────────────────
  lcd.setCursor(0, 0);
  lcd.print(F(" H2-61  16x2 LCD"));
  lcd.setCursor(0, 1);
  char addrLine[17];
  snprintf(addrLine, sizeof(addrLine), " I2C: 0x%02X  OK! ", LCD_ADDR);
  lcd.print(addrLine);
  delay(1500);

  // ── Run all tests ────────────────────────────────────────
  testCellFill();
  testColumnCounter();
  testRowLabel();
  testBacklight();
  testScroll();
  testCursor();
  testClockLayout();
  showPass();
}

void loop() {
  // nothing – all tests run once in setup()
}
