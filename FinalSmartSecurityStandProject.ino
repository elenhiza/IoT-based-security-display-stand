/*
 * ============================================================
 *   SMART DISPLAY STAND SECURITY SYSTEM
 *   Hardware : ESP32-S3-N16R8-CAM
 *   Sensors  : AS608 Fingerprint | HX711 Load Cell
 *   Output   : 20x4 I2C LCD | Active Buzzer | LED
 *   Cloud    : Telegram Bot
 *
 *   FIXES APPLIED vs original:
 *   1. FP_TX moved from GPIO 39 (input-only) to GPIO 21
 *   2. WiFi.begin() in setup() now uses ssid/password constants
 *   3. LCD uses explicit TwoWire port 0 to avoid SCCB/I2C conflict
 *   4. LoadCell.setCalFactor() now uses calibrationValue, not 1.0
 *   5. Telegram text alert uses POST + JSON body (not GET in URL)
 *   6. Photo upload uses chunked HTTP POST to avoid heap exhaustion
 *   7. calibrationValue is now actually applied
 *   8. GPIO 40 strapping-pin risk noted in comment
 * ============================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711_ADC.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <WiFi.h>
//#include <HTTPClient.h>
//#include <ArduinoJson.h>          // Add ArduinoJson v6 to your libraries
#include "esp_camera.h"
#include <time.h>
#include <vector>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ============================================================
//  PIN DEFINITIONS
// ============================================================

// HX711
#define HX711_DOUT        42
#define HX711_SCK         41

// I2C LCD — uses TwoWire port 0 (see object below)
// Camera SCCB uses I2C port 1 — no conflict
#define LCD_SDA           38
#define LCD_SCL           3

// AS608 Fingerprint (UART2)
// FIX: FP_TX changed from 39 (input-only strapping pin) to 21
#define FP_RX             14   // ESP32 RX  ← AS608 TX
#define FP_TX             21   // ESP32 TX  → AS608 RX  *** FIXED (was 39) ***

// Alarm outputs
//#define LED_PIN           38
#define BUZZER_PIN        47

// Reset button
// NOTE: GPIO 40 is a strapping pin on ESP32-S3. It is INPUT_PULLUP here
// so it's HIGH at boot (safe). Avoid pulling it LOW during power-on reset.
#define RESET_BUTTON_PIN  40

// Camera pins (ESP32-S3-N16R8-CAM)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5
#define Y2_GPIO_NUM       11
#define Y3_GPIO_NUM       9
#define Y4_GPIO_NUM       8
#define Y5_GPIO_NUM       10
#define Y6_GPIO_NUM       12
#define Y7_GPIO_NUM       18
#define Y8_GPIO_NUM       17
#define Y9_GPIO_NUM       16
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

// ============================================================
//  WIFI & CLOUD CONFIGURATION
// ============================================================
const char* ssid       = "XXXXXX";
const char* password   = "XXXXXXXX";
const char* botToken   = "85XXXXXXXX:AAEXXxxxxxxxxxxxxxxxxxxxx";
const char* chatID     = "21XXXXXXX";

// ============================================================
//  OBJECTS
// ============================================================

// FIX: LCD uses explicit TwoWire port 0 so it never conflicts
// with the camera driver which internally owns I2C port 1 (SCCB)
//TwoWire           I2C_LCD = TwoWire(0);
LiquidCrystal_I2C lcd(0x27, 20, 4);

HX711_ADC         LoadCell(HX711_DOUT, HX711_SCK);
HardwareSerial    fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);

// Telegram
WiFiClientSecure     secured_client;
UniversalTelegramBot bot(botToken, secured_client);

// ============================================================
//  SYSTEM STATES
// ============================================================
enum SystemState {
  STATE_ESTABLISHING,   // Boot — finding stable baseline
  STATE_ARMED,          // Monitoring — waiting for scan or weight change
  STATE_DISARMED,       // Authorized scan — 10s retrieval window
  STATE_ALARM,          // Intrusion detected
  STATE_STAY_DISARMED   // Item legitimately removed — waiting for reset
};

SystemState currentState = STATE_ESTABLISHING;

// ============================================================
//  WEIGHT VARIABLES
// ============================================================
// FIX: calibrationValue is now actually passed to setCalFactor()
// Run the HX711_ADC calibration example sketch first to find your value
float calibrationValue      = 100.0;

float baselineWeight        = 0;
float currentWeight         = 0;
float filteredWeight        = 0;
float previousWeight        = 0;
bool  isFirstReading        = true;
bool  baselineSet           = false;

#define WEIGHT_CHANGE_THRESHOLD   5.0   // grams
#define FILTER_STRENGTH           0.2
#define STABLE_TIME_REQUIRED      2000  // ms

unsigned long stableStartTime = 0;
unsigned long lastRefresh     = 0;

// ============================================================
//  FINGERPRINT VARIABLES
// ============================================================
bool fingerprintScanned     = false;
bool fingerprintAuthorized  = false;

// ============================================================
//  DISARM WINDOW
// ============================================================
#define RETRIEVAL_WINDOW_MS  10000  // 10 seconds
unsigned long disarmTime = 0;

// ============================================================
//  ALARM VARIABLES
// ============================================================
bool     cloudAlertSent    = false;
bool     alarmLedState     = false;
unsigned long lastAlarmToggle = 0;
unsigned long lastAlarmBlink  = 0;

// ============================================================
//  BUZZER PATTERN CONTROL
// ============================================================
struct BuzzerPattern {
  int     onMs;
  int     offMs;
  int     repeats;
  int     currentRepeat;
  bool    active;
  unsigned long lastToggle;
  bool    buzzerOn;
};

BuzzerPattern buzzer = {0, 0, 0, 0, false, 0, false};

void playPattern(int onMs, int offMs, int repeats) {
  buzzer.onMs          = onMs;
  buzzer.offMs         = offMs;
  buzzer.repeats       = repeats;
  buzzer.currentRepeat = 0;
  buzzer.active        = true;
  buzzer.lastToggle    = millis();
  buzzer.buzzerOn      = true;
  digitalWrite(BUZZER_PIN, HIGH);
}

void updateBuzzer() {
  if (!buzzer.active) return;

  unsigned long now = millis();
  if (buzzer.buzzerOn) {
    if (now - buzzer.lastToggle >= (unsigned long)buzzer.onMs) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzer.buzzerOn   = false;
      buzzer.lastToggle = now;
    }
  } else {
    if (now - buzzer.lastToggle >= (unsigned long)buzzer.offMs) {
      buzzer.currentRepeat++;
      if (buzzer.currentRepeat >= buzzer.repeats) {
        buzzer.active = false;
        return;
      }
      digitalWrite(BUZZER_PIN, HIGH);
      buzzer.buzzerOn   = true;
      buzzer.lastToggle = now;
    }
  }
}

void beepSystemReady()   { playPattern(100, 100, 2); }  // 2 short beeps
void beepAuthorized()    { playPattern(600, 0,   1); }  // 1 long beep
void beepUnauthorized()  { playPattern(80,  80,  3); }  // 3 rapid beeps
void beepAlarmReset()    { playPattern(600, 300, 2); }  // 2 long beeps

// ============================================================
//  LCD HELPERS
// ============================================================
void lcdClearLine(int row) {
  lcd.setCursor(0, row);
  lcd.print("                    ");
}

void lcdPrint(int col, int row, String text) {
  lcd.setCursor(col, row);
  lcd.print(text);
}

void lcdShowState(String line0, String line1 = "", String line2 = "", String line3 = "") {
  lcd.clear();
  lcdPrint(0, 0, line0);
  lcdPrint(0, 1, line1);
  lcdPrint(0, 2, line2);
  lcdPrint(0, 3, line3);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  //pinMode(LED_PIN,         OUTPUT);
  pinMode(BUZZER_PIN,      OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
 // digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // --------------------------------------------------------
  // 1. Camera first — it claims I2C port 1 (SCCB) internally
  // --------------------------------------------------------
  initCamera();
  Serial.println("Camera initialized.");
  delay(100);

  // --------------------------------------------------------
  // 2. LCD on I2C port 0 — safe, no conflict with camera
  // --------------------------------------------------------
  Wire.begin(LCD_SDA, LCD_SCL);   // assigns port 0 to your pins 1 & 2
  lcd.init();                      // uses Wire (port 0) internally

//I2C_LCD.begin(LCD_SDA, LCD_SCL);
 // lcd.setWire(&I2C_LCD);        // Tell the LCD lib to use our TwoWire instance
 // lcd.begin(20, 4);
  lcd.backlight();
  lcd.clear();
  lcd.print("Smart Display Stand");
  lcd.setCursor(0, 1);
  lcd.print("  Security System  ");
  lcd.setCursor(0, 2);
  lcd.print("  Initializing...  ");

  // --------------------------------------------------------
  // 3. AS608 Fingerprint on UART2 (RX=14, TX=21)
  // --------------------------------------------------------
  fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  delay(50);

  if (finger.verifyPassword()) {
    Serial.println("AS608 detected.");
  } //else {
    //Serial.println("ERROR: AS608 not found. Check wiring and FP_TX pin.");
    //lcdShowState("Fingerprint Error!", "Check wiring", "RX=14 TX=21");
   // while (1);
 // }

  // --------------------------------------------------------
  // 4. HX711 Load Cell
  // FIX: setCalFactor now uses calibrationValue (was hardcoded 1.0)
  // --------------------------------------------------------
  LoadCell.begin();
  LoadCell.start(2000, true);   // 2s stabilize + auto-tare

  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("ERROR: HX711 timeout. Check wiring.");
    lcdShowState("Load Cell Error!", "Check wiring");
    while (1);
  }

  LoadCell.setCalFactor(calibrationValue);  // *** FIXED (was 1.0) ***
  Serial.println("Load cell ready. Cal factor: " + String(calibrationValue));

  // --------------------------------------------------------
  // 5. WiFi
  // FIX: uses ssid/password constants (was hardcoded placeholder)
  // --------------------------------------------------------
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);   // *** FIXED (was "YOUR_WIFI_PASSWORD") ***

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    // WAT = UTC+1, no DST
    configTime(3600, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("\nWiFi failed — running in local-only mode.");
  }

  // --------------------------------------------------------
  // 6. Ready
  // --------------------------------------------------------
  beepSystemReady();

  lcdShowState(
    "   System  ARMED   ",
    "Scan to retrieve   ",
    "Establishing base..",
    "Please wait...     "
  );

  currentState = STATE_ESTABLISHING;
  Serial.println("--- System armed and monitoring ---");
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  updateBuzzer();
  updateWeight();

  switch (currentState) {
    case STATE_ESTABLISHING:  handleEstablishing(); break;
    case STATE_ARMED:         handleArmed();        break;
    case STATE_DISARMED:      handleDisarmed();     break;
    case STATE_ALARM:         handleAlarm();        break;
    case STATE_STAY_DISARMED: handleStayDisarmed(); break;
  }
}

// ============================================================
//  WEIGHT UPDATE
// ============================================================
void updateWeight() {
  static bool newDataReady = false;
  if (LoadCell.update()) newDataReady = true;

  if (newDataReady && (millis() - lastRefresh > 200)) {
    float rawWeight = LoadCell.getData();

    filteredWeight = isFirstReading
                   ? rawWeight
                   : (FILTER_STRENGTH * rawWeight) + ((1.0f - FILTER_STRENGTH) * filteredWeight);
    isFirstReading = false;
    currentWeight  = filteredWeight;
    newDataReady   = false;
    lastRefresh    = millis();
  }
}

// ============================================================
//  STATE: ESTABLISHING BASELINE
// ============================================================
void handleEstablishing() {
  if (fabs(filteredWeight - previousWeight) < 0.5f) {
    if (stableStartTime == 0) stableStartTime = millis();
    else if (millis() - stableStartTime > STABLE_TIME_REQUIRED) {
      baselineWeight = filteredWeight;
      baselineSet    = true;
      currentState   = STATE_ARMED;

      lcdShowState(
        "  System  ARMED   ",
        "Scan to retrieve  ",
        "Base: " + String(baselineWeight, 1) + " g",
        "Monitoring...     "
      );

      Serial.print("Baseline set: ");
      Serial.print(baselineWeight, 2);
      Serial.println(" g");
      beepSystemReady();
    }
  } else {
    stableStartTime = 0;
  }
  previousWeight = filteredWeight;
}

// ============================================================
//  STATE: ARMED
// ============================================================
void handleArmed() {
  checkFingerprint();

  float weightDiff = baselineWeight - currentWeight;

  if (weightDiff > WEIGHT_CHANGE_THRESHOLD) {
    if (fingerprintAuthorized) {
      transitionToStayDisarmed();
    } else {
      transitionToAlarm();
    }
    return;
  }

  lcdClearLine(3);
  lcdPrint(0, 3, "Wt: " + String(currentWeight, 1) + " g");
  previousWeight = filteredWeight;
}

// ============================================================
//  STATE: DISARMED (10s retrieval window)
// ============================================================
void handleDisarmed() {
  float weightDiff      = baselineWeight - currentWeight;
  unsigned long elapsed = millis() - disarmTime;
  int remaining         = (int)((RETRIEVAL_WINDOW_MS - elapsed) / 1000);

  lcdClearLine(2);
  lcdPrint(0, 2, "Time left: " + String(max(remaining, 0)) + "s   ");

  if (weightDiff > WEIGHT_CHANGE_THRESHOLD) {
    transitionToStayDisarmed();
    return;
  }

  if (elapsed > RETRIEVAL_WINDOW_MS) {
    fingerprintAuthorized = false;
    fingerprintScanned    = false;
    currentState          = STATE_ARMED;

    lcdShowState(
      "  System  ARMED   ",
      "Scan to retrieve  ",
      "Base: " + String(baselineWeight, 1) + " g",
      "Monitoring...     "
    );
    Serial.println("Retrieval window expired. Re-armed.");
  }

  previousWeight = filteredWeight;
}

// ============================================================
//  STATE: ALARM
// ============================================================
void handleAlarm() {
  // Flash LED and buzzer
  if (millis() - lastAlarmToggle > 250) {
    alarmLedState = !alarmLedState;
    //digitalWrite(LED_PIN,    alarmLedState ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, alarmLedState ? HIGH : LOW);
    lastAlarmToggle = millis();
  }

  // Blink INTRUDER message
  if (millis() - lastAlarmBlink > 500) {
    lcdClearLine(1);
    lcdPrint(0, 1, (millis() / 500) % 2 == 0
                   ? "   !! INTRUDER !!  "
                   : "                   ");
    lastAlarmBlink = millis();
  }

  // Send cloud alert once
  if (!cloudAlertSent) {
    cloudAlertSent = true;
    camera_fb_t* image = captureImage();
     sendTelegramTextAlert(baselineWeight, currentWeight);
  secured_client.stop();   // <-- add this line
  delay(500); 
    if (image) {
      sendTelegramPhoto(image);
      esp_camera_fb_return(image);
    } else {
      // Send text only if camera failed
      //sendTelegramTextAlert(baselineWeight, currentWeight);
       Serial.println("Camera capture failed — text alert already sent.");
    }
  }

  // Authorized fingerprint resets alarm
  int fpResult = getFingerprintID();
  if (fpResult > 0) {
    transitionFromAlarm();
  }
}

// ============================================================
//  STATE: STAY DISARMED
// ============================================================
void handleStayDisarmed() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    delay(50);
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      transitionToArmed();
    }
  }
}

// ============================================================
//  STATE TRANSITIONS
// ============================================================
void transitionToAlarm() {
  currentState          = STATE_ALARM;
  cloudAlertSent        = false;
  fingerprintAuthorized = false;
  fingerprintScanned    = false;

  //digitalWrite(LED_PIN, HIGH);
  lcdShowState(
    "!! SECURITY ALERT !!",
    "   !! INTRUDER !!   ",
    "Scan fingerprint    ",
    "to reset alarm      "
  );
  Serial.println("ALARM: Unauthorized access detected!");
  beepUnauthorized();
}

void transitionToDisarmed() {
  currentState = STATE_DISARMED;
  disarmTime   = millis();

  lcdShowState(
    "  Access Granted  ",
    "Retrieve your item",
    "Time left: 10s    ",
    "                  "
  );
  Serial.println("Authorized. 10s retrieval window open.");
  beepAuthorized();
}

void transitionToStayDisarmed() {
  currentState          = STATE_STAY_DISARMED;
  fingerprintAuthorized = false;
  fingerprintScanned    = false;
  //digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  lcdShowState(
    " Item Removed OK  ",
    "System Disarmed   ",
    "Press reset button",
    "to re-arm system  "
  );
  Serial.println("Item removed legitimately. System disarmed.");
}

void transitionFromAlarm() {
  currentState          = STATE_STAY_DISARMED;
  cloudAlertSent        = false;
  fingerprintAuthorized = false;
  fingerprintScanned    = false;
  //digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  lcdShowState(
    "  Alarm  Reset    ",
    "System Disarmed   ",
    "Press reset button",
    "to re-arm system  "
  );
  Serial.println("Alarm reset by authorized fingerprint.");
  beepAlarmReset();
}

void transitionToArmed() {
  currentState          = STATE_ESTABLISHING;
  baselineSet           = false;
  stableStartTime       = 0;
  fingerprintAuthorized = false;
  fingerprintScanned    = false;
  isFirstReading        = true;

  lcdShowState(
    "Place item on stand",
    "Establishing base..",
    "",
    ""
  );
  Serial.println("Reset pressed. Re-establishing baseline...");
  beepSystemReady();
}

// ============================================================
//  FINGERPRINT
// ============================================================
void checkFingerprint() {
  int fpID = getFingerprintID();

  if (fpID > 0) {
    fingerprintScanned    = true;
    fingerprintAuthorized = true;
    Serial.println("Fingerprint authorized. ID: " + String(fpID));
    transitionToDisarmed();

  } else if (fpID == -1) {
    fingerprintScanned    = true;
    fingerprintAuthorized = false;
    Serial.println("Fingerprint NOT recognized.");

    lcdClearLine(1);
    lcdPrint(0, 1, "!! Wrong Print !!  ");
    beepUnauthorized();
    delay(1000);
    lcdClearLine(1);
    lcdPrint(0, 1, "Scan to retrieve  ");
  }
}

int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return 0;
  if (p != FINGERPRINT_OK)       return 0;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return 0;

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    return finger.fingerID;
  } else {
    return -1;
  }
}

// ============================================================
//  CAMERA
// ============================================================
void initCamera() {
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sccb_sda  = SIOD_GPIO_NUM;
  config.pin_sccb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;
  config.frame_size    = FRAMESIZE_VGA;
  config.jpeg_quality  = 12;
  config.fb_count      = 1;
  config.sccb_i2c_port = 1;  // Camera owns I2C port 1; LCD uses port 0

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    // LCD not yet initialized here, so log to serial only
    while (1);
  }
  Serial.println("Camera ready.");
}

camera_fb_t* captureImage() {
  // Discard warm-up frames for auto-exposure to settle
  for (int i = 0; i < 3; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }
  return esp_camera_fb_get();
}

// ============================================================
//  TELEGRAM — TEXT ALERT
//  FIX: Uses POST with JSON body instead of GET with URL params.
//  Handles special characters and long messages correctly.
// ============================================================

void sendTelegramTextAlert(float baseline, float current) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — skipping alert.");
    return;
  }

  time_t now;
  time(&now);
  char timeStr[25];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));

  String msg = "SECURITY ALERT\n";
  msg += "Time: " + String(timeStr) + "\n";
  msg += "Baseline: " + String(baseline, 1) + " g\n";
  msg += "Current:  " + String(current,  1) + " g\n";
  msg += "Removed:  " + String(baseline - current, 1) + " g\n";
  msg += "Photo follows...";

  secured_client.setInsecure(); // skip SSL cert verification
  bot.sendMessage(chatID, msg, "");
  Serial.println("Telegram text alert sent.");
}

// ============================================================
//  TELEGRAM — PHOTO
//  FIX: Uses chunked streaming POST to avoid large heap allocation.
//  The original code copied the entire JPEG into a std::vector
//  which could exhaust heap on large frames.
// ============================================================

void sendTelegramPhoto(camera_fb_t* fb) {
  if (WiFi.status() != WL_CONNECTED || !fb || fb->len == 0) {
    Serial.println("Skipping photo — no WiFi or empty frame.");
    return;
  }

  secured_client.setInsecure();

  //String url = "https://api.telegram.org/bot" + String(botToken) + "/sendPhoto";

  // Build multipart body in a String (fine for JPEG at VGA ~20-40KB)
  String head = "--boundary\r\n"
                "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  head += String(chatID);
  head += "\r\n--boundary\r\n"
          "Content-Disposition: form-data; name=\"photo\"; filename=\"alert.jpg\"\r\n"
          "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--boundary--\r\n";

  uint32_t totalLen = head.length() + fb->len + tail.length();

  if (!secured_client.connect("api.telegram.org", 443)) {
    Serial.println("Telegram photo: connection failed.");
    return;
  }

  // Send HTTP request manually over the secure client
  secured_client.println("POST /bot" + String(botToken) + "/sendPhoto HTTP/1.1");
  secured_client.println("Host: api.telegram.org");
  secured_client.println("Content-Type: multipart/form-data; boundary=boundary");
  secured_client.println("Content-Length: " + String(totalLen));
  secured_client.println("Connection: close");
  secured_client.println();
  secured_client.print(head);

  // Send JPEG in chunks
  uint8_t* buf = fb->buf;
  size_t remaining = fb->len;
  while (remaining > 0) {
    size_t toSend = min(remaining, (size_t)1024);
    secured_client.write(buf, toSend);
    buf += toSend;
    remaining -= toSend;
  }

  secured_client.print(tail);

  // Wait briefly for response
  unsigned long timeout = millis();
  while (secured_client.connected() && millis() - timeout < 5000) {
    if (secured_client.available()) {
      String line = secured_client.readStringUntil('\n');
      Serial.println(line);  // print first line — should contain "200 OK"
      break;
    }
  }
  secured_client.stop();
  Serial.println("Telegram photo sent.");
}
