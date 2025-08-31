/*
  Mini Logger - Full Sketch
  - 8.3-safe filenames: /LYYMMDDxx.CSV
  - SD hotplug detection
  - Debounced button (short click toggles logging)
  - Buffered logging flushed every FLUSH_INTERVAL_SECONDS
  - OLED shows local time based on GPS longitude

  + Added:
    - Hall-effect RPM on IO1
    - 2 pulses/rev
    - RPM computed at 10 Hz (RPM_UPDATE_HZ)
    - Ones digit forced to 0
    - Same RPM used for OLED + CSV
    - RPM drawn in large font just below MPH, only if > 0
*/

#include <Wire.h>
#include <Adafruit_SH110X.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <SD.h>

// ==================== CONFIG ====================
// Pins
#define SDA_PIN 20
#define SCL_PIN 21
#define GPS_RX  3    // GPS TX -> ESP32 RX
#define GPS_TX  4    // GPS RX <- ESP32 TX
#define SD_CS   9
#define SD_MOSI 8
#define SD_CLK  7
#define SD_MISO 6
#define BUTTON_PIN 10  // Button to GND (INPUT_PULLUP)

// Hall effect RPM config
#define HALL_PIN 1            // Hall sensor signal on IO1
#define PULSES_PER_REV 2      // 2 pulses per revolution
#define RPM_UPDATE_HZ 10      // compute RPM at 10 Hz (every 100 ms)

// Timing for button handling (ms)
#define BUTTON_DEBOUNCE_DELAY 50
#define BUTTON_LONG_PRESS_TIME 2000

// Logging timing
#define FLUSH_INTERVAL_SECONDS 10
#define LOG_INTERVAL_SECONDS 1
#define LOG_LINE_SIZE 64
#define LOG_LINES_MAX ((FLUSH_INTERVAL_SECONDS) / (LOG_INTERVAL_SECONDS))
const unsigned long BUFFER_FLUSH_INTERVAL_MS = (unsigned long)FLUSH_INTERVAL_SECONDS * 1000UL;

// Display & GPS objects
Adafruit_SH1107 display(128, 128, &Wire);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

// ==================== ICONS ====================
const unsigned char sdIcon16x16[] PROGMEM = {
  0xFF,0xFF,0x80,0x01,0xBF,0xFD,0xBF,0xFD,0xBF,0xFD,0xBF,0xFD,0xBF,0xFD,0x80,0x01,
  0x8F,0xF1,0x88,0x11,0x88,0x11,0x88,0x11,0x88,0x11,0x88,0x11,0x88,0x11,0x88,0x11,
  0xFF,0xFF
};
const unsigned char dot16x16[] PROGMEM = {
  0x00,0x00,0x07,0xE0,0x1F,0xF8,0x3F,0xFC,0x7F,0xFE,0x7F,0xFE,0x7F,0xFE,0x7F,0xFE,
  0x7F,0xFE,0x7F,0xFE,0x3F,0xFC,0x1F,0xF8,0x07,0xE0,0x00,0x00,0x00,0x00,0x00,0x00
};

// ==================== STATE ====================
bool sdInserted = false;
bool isLogging = false;
unsigned long lastBlinkTime = 0;
bool blinkState = false;
int lastLoggedSecond = -1;

char logBuffer[LOG_LINE_SIZE * LOG_LINES_MAX];
size_t logLinesCount = 0;

String currentLogFileName = "";
File logFile;
unsigned long lastBufferFlushMillis = 0;

// RPM state
int RPM = 0; // <-- used for BOTH OLED + CSV (ones digit forced to 0)
volatile unsigned long pulseCount = 0;
unsigned long lastRPMSampleMillis = 0;

// Button handling
bool buttonJustClicked = false;
bool buttonLongPressed = false;
unsigned long buttonPressStart = 0;
unsigned long buttonLastChange = 0;
bool lastButtonReading = HIGH; // raw last reading
bool buttonStableState = HIGH; // debounced stable state

// Toggle cooldown
unsigned long lastToggleMillis = 0;
const unsigned long TOGGLE_COOLDOWN_MS = 5000; // 5 seconds

// Bottom message display
String bottomMessage = "";
unsigned long bottomMessageTimestamp = 0;
const unsigned long BOTTOM_MESSAGE_DURATION_MS = 3000; // 3 seconds

// Display refresh control
unsigned long lastDisplayUpdateMillis = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL_MS = 200;

// SD hotplug detection control
unsigned long lastSDCheckMillis = 0;
const unsigned long SD_CHECK_INTERVAL_MS = 2000;

// File created message control
bool showFileCreatedMsg = false;
unsigned long fileCreatedMsgStart = 0;
const unsigned long FILE_CREATED_MSG_DURATION_MS = 3000; // 3 seconds

// GPS & date/time helpers
struct LocalTime { int hour; int minute; int second; };

unsigned long loggingStartMillis = 0; // <-- fixed declaration

// ==================== RPM ISR ====================
void IRAM_ATTR hallISR() { pulseCount++; }

// Convert GPS time + longitude to local time (integer hours offset)
LocalTime getLocalTime(TinyGPSDate date, TinyGPSTime time, double longitude) {
  // Compute timezone from longitude (negative west)
  int timezoneOffsetHours = -5; // standard time
  // simple DST approx: March second Sunday → Nov first Sunday
  int month = date.month();
  int day = date.day();
  int hour = time.hour();
  if (month>3 && month<11) timezoneOffsetHours = -5;
  else if (month==3 && day>=8) timezoneOffsetHours = -4;  // crude approximation
  else if (month==11 && day<=7) timezoneOffsetHours = -4;
  int h = time.hour() + timezoneOffsetHours;
  int m = time.minute();
  int s = time.second();
  
  // Wrap seconds
  if (s >= 60) { s -= 60; m += 1; }
  if (s < 0)   { s += 60; m -= 1; }
  // Wrap minutes
  if (m >= 60) { m -= 60; h += 1; }
  if (m < 0)   { m += 60; h -= 1; }
  // Wrap hours
  if (h >= 24) h -= 24;
  if (h < 0)   { h += 24; }
  
  return LocalTime{h, m, s};
}

void format12Hour(int hour24, int &hour12, const char* &amPm) {
  if (hour24 == 0) { hour12 = 12; amPm = "AM"; }
  else if (hour24 < 12) { hour12 = hour24; amPm = "AM"; }
  else if (hour24 == 12) { hour12 = 12; amPm = "PM"; }
  else { hour12 = hour24 - 12; amPm = "PM"; }
}

// ==================== FILENAME (8.3 safe) ====================
// Produces LYYMMDDxx.CSV where xx is 00..99
void generateNextAvailableLogFileName(char *outFilename, size_t outSize, int yy, int mm, int dd) {
  for (int i = 0; i < 100; ++i) {
    snprintf(outFilename, outSize, "/L%02d%02d%02d%02d.CSV", yy, mm, dd, i);
    if (!SD.exists(outFilename)) return;
  }
  snprintf(outFilename, outSize, "/L%02d%02d%02d99.CSV", yy, mm, dd);
}

bool openLogFileNew() {
  if (!sdInserted) return false;
  int yy = gps.date.isValid() ? gps.date.year() % 100 : 0;
  int mm = gps.date.isValid() ? gps.date.month() : 0;
  int dd = gps.date.isValid() ? gps.date.day() : 0;

  char fn[20];
  generateNextAvailableLogFileName(fn, sizeof(fn), yy, mm, dd);
  currentLogFileName = String(fn);

  logFile = SD.open(currentLogFileName.c_str(), FILE_WRITE);
  if (!logFile) {
    Serial.print("Failed to create new log file: "); Serial.println(currentLogFileName);
    return false;
  }
  Serial.print("New log file created: "); Serial.println(currentLogFileName);
  logFile.println("lat,lon,speed_mph,UTC_datetime,RPM");
  logFile.flush();
  return true;
}

bool openLogFileIfNeeded() {
  if (!sdInserted) return false;
  if (logFile) return true;
  if (currentLogFileName.length() > 0) {
    logFile = SD.open(currentLogFileName.c_str(), FILE_WRITE);
    if (logFile) return true;
  }
  return false;
}

// ==================== LOG BUFFERING & FLUSH ====================
void flushLogBuffer() {
  if (logLinesCount == 0) return;

  if (!sdInserted) { bottomMessage = "No SD card!"; bottomMessageTimestamp = millis(); return; }
  if (!openLogFileIfNeeded()) { bottomMessage = "SD File Error"; bottomMessageTimestamp = millis(); return; }

  size_t bytesToWrite = logLinesCount * LOG_LINE_SIZE;
  size_t wrote = logFile.write((const uint8_t*)logBuffer, bytesToWrite);

  if (wrote != bytesToWrite) {
    if (logFile) logFile.close();
    if (openLogFileIfNeeded()) logFile.write((const uint8_t*)logBuffer, bytesToWrite);
    else { isLogging = false; bottomMessage = "SD Write Error"; bottomMessageTimestamp = millis(); }
  } else {
    logFile.flush();
    logLinesCount = 0;
    bottomMessage = "Writing...";
    bottomMessageTimestamp = millis();
  }
}

void bufferLogLine() {
  char line[LOG_LINE_SIZE];
  int speed_mph = gps.speed.isValid() ? (int)(gps.speed.mph() + 0.5) : -1;
  int len = snprintf(line, sizeof(line),
    "%.6f,%.6f,%d,%04d-%02d-%02d %02d:%02d:%02d,%d\n",
    gps.location.isValid() ? gps.location.lat() : 0.0,
    gps.location.isValid() ? gps.location.lng() : 0.0,
    speed_mph,
    gps.date.isValid() ? gps.date.year() : 0,
    gps.date.isValid() ? gps.date.month() : 0,
    gps.date.isValid() ? gps.date.day() : 0,
    gps.time.isValid() ? gps.time.hour() : 0,
    gps.time.isValid() ? gps.time.minute() : 0,
    gps.time.isValid() ? gps.time.second() : 0,
    RPM // <- SAME RPM as OLED, ones digit already forced to 0
  );

  if (len < 0) return;
  if ((size_t)len > LOG_LINE_SIZE) { len = LOG_LINE_SIZE; line[LOG_LINE_SIZE-1] = '\n'; }
  for (int i=len;i<(int)LOG_LINE_SIZE;++i) line[i]=' ';

  if (logLinesCount >= LOG_LINES_MAX) flushLogBuffer();
  if (logLinesCount < LOG_LINES_MAX) {
    memcpy(&logBuffer[logLinesCount*LOG_LINE_SIZE], line, LOG_LINE_SIZE);
    logLinesCount++;
  }
}

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplayLogging() {
  unsigned long now = millis();

  if (showFileCreatedMsg) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.setTextSize(1);
    display.print("File created:");
    display.setCursor(0,12);
    display.print(currentLogFileName);
    display.display();
    if (now - fileCreatedMsgStart >= FILE_CREATED_MSG_DURATION_MS) showFileCreatedMsg=false;
    return;
  }

  if (now - lastBlinkTime > 500) { blinkState = !blinkState; lastBlinkTime=now; }

  display.clearDisplay();
  display.setCursor(0,0);

  if (gps.time.isValid() && gps.date.isValid()) {
    double lon = gps.location.isValid() ? gps.location.lng() : 0.0;
    LocalTime lt = getLocalTime(gps.date, gps.time, gps.location.isValid() ? gps.location.lng() : 0.0);
    int h12; const char* ampm;
    format12Hour(lt.hour,h12,ampm);
    display.printf("%02d:%02d:%02d %s\n",h12,lt.minute,lt.second,ampm);
  } else display.println("--:--:--");

  bool hasFixLocal = gps.location.isValid() && gps.location.age()<3000;
  display.printf("Fix: %s\n", hasFixLocal ? "YES" : "NO");

  if (gps.satellites.isValid()) display.printf("Sats: %d\n", gps.satellites.value());
  else display.println("Sats: --");

  // MPH (large)
  display.setTextSize(3);
  display.setCursor(0, 24); // explicitly position MPH line
  if (gps.speed.isValid()) display.printf("MPH:%3d", (int)(gps.speed.mph()+0.5));
  else display.println("MPH: --");

  // RPM (large, only if > 0), ones digit already 0
  if (RPM > 0) {
    display.setCursor(0, 52); // just below MPH (24px tall per line at size 3)
    display.printf("RPM:%4d", RPM);
  }

  display.setTextSize(1);

  // Icons
  if (sdInserted) {
    if (isLogging && blinkState && gps.location.isValid())
      display.drawBitmap(83,0,dot16x16,16,16,SH110X_WHITE);
    display.drawBitmap(112,0,sdIcon16x16,16,16,SH110X_WHITE);
  }

  if (bottomMessage.length()>0 && millis()-bottomMessageTimestamp<BOTTOM_MESSAGE_DURATION_MS) {
    display.setCursor(0,100);
    display.print(bottomMessage);
  }

  display.display();
}

// ==================== BUTTON HANDLING ====================
void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  unsigned long now=millis();
  if (reading!=lastButtonReading) buttonLastChange=now;
  if ((now-buttonLastChange)>BUTTON_DEBOUNCE_DELAY) {
    if (reading!=buttonStableState) {
      buttonStableState=reading;
      if (buttonStableState==LOW) buttonPressStart=now;
      else {
        unsigned long pressLen=now-buttonPressStart;
        if (pressLen>=BUTTON_LONG_PRESS_TIME) { buttonLongPressed=true; buttonJustClicked=false; }
        else { buttonJustClicked=true; buttonLongPressed=false; }
      }
    }
  }
  lastButtonReading=reading;
}

bool hasFix() { return gps.location.isValid() && gps.location.age()<3000 && gps.satellites.isValid() && gps.satellites.value()>=3; }

void checkSDCardPresence() {
  unsigned long now=millis();
  if (now-lastSDCheckMillis<SD_CHECK_INTERVAL_MS) return;
  lastSDCheckMillis=now;
  bool currentlyInserted = SD.begin(SD_CS);
  if (currentlyInserted && !sdInserted) {
    sdInserted=true;
    bottomMessage="SD Inserted"; bottomMessageTimestamp=now;
  } else if (!currentlyInserted && sdInserted) {
    sdInserted=false;
    bottomMessage="SD Removed"; bottomMessageTimestamp=now;
    if (isLogging) { if (logLinesCount>0) flushLogBuffer(); if (logFile) logFile.close(); isLogging=false; }
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Hall sensor interrupt
  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), hallISR, FALLING);
  lastRPMSampleMillis = millis();

  Wire.begin(SDA_PIN,SCL_PIN);
  display.begin(0x3C,true);
  display.setRotation(0);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0,0);
  display.println("Initializing...");
  display.display();

  gpsSerial.begin(9600,SERIAL_8N1,GPS_RX,GPS_TX);
  SPI.begin(SD_CLK,SD_MISO,SD_MOSI,SD_CS);
  if (SD.begin(SD_CS)) { sdInserted=true; bottomMessage="SD Ready"; bottomMessageTimestamp=millis(); }
  else { sdInserted=false; bottomMessage="No SD card!"; bottomMessageTimestamp=millis(); display.clearDisplay(); display.setCursor(0,0); display.println("No SD card!"); display.display(); }
  lastBufferFlushMillis=millis();
  isLogging=false;
  bottomMessage=""; bottomMessageTimestamp=0;
  updateDisplayLogging();
}

// ==================== MAIN LOOP ====================
void loop() {
  handleButton();
  checkSDCardPresence();

  unsigned long now=millis();

  // ==== RPM update at 10 Hz (ones digit forced to 0) ====
  const unsigned long rpmInterval = 1000UL / RPM_UPDATE_HZ; // e.g., 100 ms
  if (now - lastRPMSampleMillis >= rpmInterval) {
    unsigned long elapsed = now - lastRPMSampleMillis;
    lastRPMSampleMillis = now;

    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float rpmRaw = (elapsed > 0) ? ((float)pulses / PULSES_PER_REV) * (60000.0f / (float)elapsed) : 0.0f;
    int rpmInt = (int)rpmRaw;
    RPM = (rpmInt / 10) * 10; // force ones digit to 0
  }

  if (buttonJustClicked) {
    if ((now-lastToggleMillis)>=TOGGLE_COOLDOWN_MS) {
      if (!sdInserted) { bottomMessage="No SD card!"; bottomMessageTimestamp=now; }
      else {
        if (!isLogging) {
          if (openLogFileNew()) {
            logFile.flush();
            isLogging=true;
            lastLoggedSecond=-1;
            lastBufferFlushMillis=now;
            loggingStartMillis=now;
            lastToggleMillis=now;
            showFileCreatedMsg=true;
            fileCreatedMsgStart=now;
            bottomMessage=""; bottomMessageTimestamp=0;
          } else { bottomMessage="File error!"; bottomMessageTimestamp=now; }
        } else {
          if (logLinesCount>0) flushLogBuffer();
          if (logFile) logFile.close();
          isLogging=false;
          lastToggleMillis=now;
          bottomMessage="Saved as: "+currentLogFileName;
          bottomMessageTimestamp=now;
        }
      }
    }
    buttonJustClicked=false;
  }

  if (buttonLongPressed) { bottomMessage="Long press"; bottomMessageTimestamp=now; buttonLongPressed=false; }

  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  if (isLogging && gps.time.isValid() && gps.date.isValid() && hasFix() && sdInserted) {
    int currentSecond=gps.time.second();
    if (currentSecond!=lastLoggedSecond) { lastLoggedSecond=currentSecond; bufferLogLine(); }
  }

  if (millis()-lastBufferFlushMillis>=BUFFER_FLUSH_INTERVAL_MS) {
    if (logLinesCount>0 && sdInserted && isLogging) {
      if (openLogFileIfNeeded()) flushLogBuffer();
    }
    lastBufferFlushMillis=millis();
  }

  if (now-lastDisplayUpdateMillis>DISPLAY_UPDATE_INTERVAL_MS) {
    updateDisplayLogging();
    lastDisplayUpdateMillis=now;
  }
}