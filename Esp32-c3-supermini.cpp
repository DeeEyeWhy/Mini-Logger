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

// Make these easy to change:
#define FLUSH_INTERVAL_SECONDS 5  // buffer flush interval
#define LOG_INTERVAL_SECONDS 1    // log frequency in seconds
#define LOG_LINE_SIZE 64          // fixed bytes reserved per line
#define LOG_LINES_MAX (FLUSH_INTERVAL_SECONDS / LOG_INTERVAL_SECONDS) // computed

// Derived
const unsigned long BUFFER_FLUSH_INTERVAL_MS = (unsigned long)FLUSH_INTERVAL_SECONDS * 1000UL;

// ==================== HARDWARE OBJECTS ====================
Adafruit_SH1107 display(128, 128, &Wire);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
File logFile;

// ==================== ICONS ====================
// 16x16 SD icon (user provided)
const unsigned char sdIcon16x16[] PROGMEM = {
  0xFF, 0xFF,
  0x80, 0x01,
  0xBF, 0xFD,
  0xBF, 0xFD,
  0xBF, 0xFD,
  0xBF, 0xFD,
  0xBF, 0xFD,
  0x80, 0x01,
  0x8F, 0xF1,
  0x88, 0x11,
  0x88, 0x11,
  0x88, 0x11,
  0x88, 0x11,
  0x88, 0x11,
  0x88, 0x11,
  0xFF, 0xFF
};

// 16x16 filled dot for logging indicator
const unsigned char dot16x16[] PROGMEM = {
  0x00,0x00,
  0x07,0xE0,
  0x1F,0xF8,
  0x3F,0xFC,
  0x7F,0xFE,
  0x7F,0xFE,
  0x7F,0xFE,
  0x7F,0xFE,
  0x7F,0xFE,
  0x7F,0xFE,
  0x3F,0xFC,
  0x1F,0xF8,
  0x07,0xE0,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};

// ==================== STATE ====================
bool sdInserted = false;           // checked once at boot
bool isLogging = false;            // true when buffer contains data waiting to flush
unsigned long lastBlinkTime = 0;
bool blinkState = false;
int lastLoggedSecond = -1;

char logBuffer[LOG_LINE_SIZE * LOG_LINES_MAX]; // contiguous buffer
size_t logLinesCount = 0;

String currentLogFileName = "";
TinyGPSDate lastDate;
TinyGPSTime lastTime;
unsigned long lastBufferFlushMillis = 0;

// timezone conversion (display only)
const int timezoneOffsetHours = -5;
const int timezoneOffsetMinutes = 0;

// RPM placeholder (your code should update this)
int RPM = 0;

// ==================== UTIL: local time conversion & formatting ====================
struct LocalTime { int hour; int minute; int second; };

LocalTime getLocalTime(TinyGPSDate date, TinyGPSTime time) {
  int h = time.hour() + timezoneOffsetHours;
  int m = time.minute() + timezoneOffsetMinutes;
  int s = time.second();
  if (s >= 60) { s -= 60; m += 1; }
  if (s < 0)   { s += 60; m -= 1; }
  if (m >= 60) { m -= 60; h += 1; }
  if (m < 0)   { m += 60; h -= 1; }
  if (h >= 24) h -= 24;
  if (h < 0)   h += 24;
  return LocalTime{h, m, s};
}

void format12Hour(int hour24, int &hour12, const char* &amPm) {
  if (hour24 == 0) { hour12 = 12; amPm = "AM"; }
  else if (hour24 < 12) { hour12 = hour24; amPm = "AM"; }
  else if (hour24 == 12) { hour12 = 12; amPm = "PM"; }
  else { hour12 = hour24 - 12; amPm = "PM"; }
}

// ==================== RANDOM 4 LETTER CODE GENERATOR ====================
String randomFourLetterCode() {
  String code = "";
  for (int i = 0; i < 4; i++) {
    char letter = 'A' + random(0, 26); // Random letter A-Z
    code += letter;
  }
  return code;
}

// ==================== FILENAME HANDLING WITH RANDOM CODE ====================
// returns e.g. "/2025_08_11_ABCD.csv"
String generateLogFileNameWithRandomCode(TinyGPSDate date) {
  char base[32];
  snprintf(base, sizeof(base), "/%04d_%02d_%02d_", date.year(), date.month(), date.day());

  // try up to 100 random codes to find unused filename
  for (int i = 0; i < 100; ++i) {
    String code = randomFourLetterCode();
    char filename[48];
    snprintf(filename, sizeof(filename), "%s%s.csv", base, code.c_str());

    if (!SD.exists(filename)) {
      return String(filename);
    }
  }

  // fallback filename if all attempts fail
  snprintf(base, sizeof(base), "/%04d_%02d_%02d_default.csv", date.year(), date.month(), date.day());
  return String(base);
}

// ==================== WRITE BLANK LINES TO FILE ====================
void writeBlankLines(File &file, int count = 10) {
  for (int i = 0; i < count; i++) {
    // Format: lat,lon,speed_mph,UTC_date,UTC_time,RPM - zeros
    file.println("0.000000,0.000000,0,0000-00-00 00:00:00,0");
  }
  file.flush();
}

// ==================== OPEN FILE FOR WRITING AND CREATE HEADER ====================
bool openLogFileIfNeeded() {
  if (!sdInserted) return false;
  if (logFile) return true; // already open
  if (currentLogFileName == "") {
    currentLogFileName = generateLogFileNameWithRandomCode(gps.date);
  }
  logFile = SD.open(currentLogFileName.c_str(), FILE_WRITE);
  if (!logFile) {
    Serial.println("ERROR: could not open log file for writing");
    return false;
  }
  // if new file (size 0) write header
  if (logFile.size() == 0) {
    logFile.println("lat,lon,speed_mph,UTC_date,UTC_time,RPM");
    logFile.flush();
    writeBlankLines(logFile, 10);
  }
  return true;
}

// ==================== FLUSH BUFFER TO SD (keeps file open, uses flush()) ====================
void flushLogBuffer() {
  if (logLinesCount == 0) return;

  if (!openLogFileIfNeeded()) {
    Serial.println("Flush aborted: file not available");
    return;
  }

  size_t bytesToWrite = logLinesCount * LOG_LINE_SIZE;
  size_t wrote = logFile.write((const uint8_t*)logBuffer, bytesToWrite);
  if (wrote != bytesToWrite) {
    Serial.print("Warning: wrote "); Serial.print(wrote); Serial.print(" of "); Serial.println(bytesToWrite);
  }
  logFile.flush();
  Serial.print("Flushed "); Serial.print(bytesToWrite); Serial.println(" bytes to SD (flush)");
  logLinesCount = 0;
  isLogging = false;
  lastDate = gps.date;
  lastTime = gps.time;
}

// ==================== BUFFER A SINGLE LOG LINE (fixed width) ====================
void bufferLogLine() {
  char line[LOG_LINE_SIZE];
  int speed_mph = (int)(gps.speed.mph() + 0.5);
  int len = snprintf(line, sizeof(line),
    "%.6f,%.6f,%d,%04d-%02d-%02d %02d:%02d:%02d,%d\n",
    gps.location.lat(),
    gps.location.lng(),
    speed_mph,
    gps.date.year(),
    gps.date.month(),
    gps.date.day(),
    gps.time.hour(),
    gps.time.minute(),
    gps.time.second(),
    RPM
  );

  if (len < 0) {
    Serial.println("Formatting error");
    return;
  }
  if ((size_t)len > LOG_LINE_SIZE) {
    len = LOG_LINE_SIZE;
    line[LOG_LINE_SIZE - 1] = '\n';
  }

  for (int i = len; i < (int)LOG_LINE_SIZE; ++i) line[i] = ' ';

  if (logLinesCount >= LOG_LINES_MAX) {
    flushLogBuffer();
  }

  memcpy(&logBuffer[logLinesCount * LOG_LINE_SIZE], line, LOG_LINE_SIZE);
  logLinesCount++;
  isLogging = true;
}

// ==================== DISPLAY UPDATE (separated, lightweight) ====================
void updateDisplay() {
  if (millis() - lastBlinkTime > 500) {
    blinkState = !blinkState;
    lastBlinkTime = millis();
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  if (gps.time.isValid() && gps.date.isValid()) {
    LocalTime lt = getLocalTime(gps.date, gps.time);
    int h12; const char* ampm;
    format12Hour(lt.hour, h12, ampm);
    display.printf("%02d:%02d:%02d %s\n", h12, lt.minute, lt.second, ampm);
  } else {
    display.println("--:--:--");
  }

  bool hasFix = gps.location.isValid() && gps.location.age() < 3000;
  display.printf("Fix: %s\n", hasFix ? "YES" : "NO");

  if (gps.satellites.isValid()) {
    display.printf("Sats: %d\n", gps.satellites.value());
  } else {
    display.println("Sats: --");
  }

  display.setTextSize(3);
  if (gps.speed.isValid()) {
    int speedWhole = (int)(gps.speed.mph() + 0.5);
    display.printf("MPH:%3d", speedWhole);
  } else {
    display.println("MPH: --");
  }
  display.setTextSize(1);

  if (sdInserted) {
    if (isLogging && blinkState && gps.location.isValid()) {
      display.drawBitmap(83, 0, dot16x16, 16, 16, SH110X_WHITE);
    }
    display.drawBitmap(112, 0, sdIcon16x16, 16, 16, SH110X_WHITE);
  }

  display.display();
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));  // Seed random number generator

  Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(0x3C, true);
  display.setRotation(0);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);

  if (SD.begin(SD_CS)) {
    Serial.println("SD card initialized.");
    sdInserted = true;

    currentLogFileName = generateLogFileNameWithRandomCode(gps.date);
    logFile = SD.open(currentLogFileName.c_str(), FILE_WRITE);
    if (logFile) {
      Serial.print("Created log file: "); Serial.println(currentLogFileName);
      if (logFile.size() == 0) {
        logFile.println("lat,lon,speed_mph,UTC_date,UTC_time,RPM");
        logFile.flush();
        writeBlankLines(logFile, 10);
      }
    } else {
      Serial.println("Failed to create log file on startup.");
      sdInserted = false;
    }

    // Show boot screen with filename for 3 seconds
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Log file created:");
    display.println(currentLogFileName);
    display.display();
    delay(3000);

  } else {
    Serial.println("No SD card found. Logging disabled.");
    sdInserted = false;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("No SD card!");
    display.display();
  }

  lastBufferFlushMillis = millis();
}

// ==================== MAIN LOOP ====================
void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  if (gps.time.isValid() && gps.date.isValid()) {
    int currentSecond = gps.time.second();
    if (currentSecond != lastLoggedSecond) {
      lastLoggedSecond = currentSecond;
      if (gps.location.isValid() && sdInserted) {
        bufferLogLine();
      }
    }
  }

  if (millis() - lastBufferFlushMillis >= BUFFER_FLUSH_INTERVAL_MS) {
    if (logLinesCount > 0 && sdInserted) {
      if (!openLogFileIfNeeded()) {
        Serial.println("Failed to open log file at flush time");
      } else {
        flushLogBuffer();
      }
    }
    lastBufferFlushMillis = millis();
  }

  updateDisplay();
}
