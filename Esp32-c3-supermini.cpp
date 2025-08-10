#include <Wire.h>
#include <Adafruit_SH110X.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <SD.h>

// ===== Pinout =====
// OLED pins
#define SDA_PIN 20
#define SCL_PIN 21
// GPS pins
#define GPS_RX  3  // GPS TX → ESP32 RX
#define GPS_TX  4  // GPS RX ← ESP32 TX
// MicroSD pins (3.3V logic)
#define SD_CS   9
#define SD_MOSI 8
#define SD_CLK  7
#define SD_MISO 6

// ===== Display Setup =====
Adafruit_SH1107 display(128, 128, &Wire);

// ===== GPS Setup =====
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

// Track last GPS data time to determine if GPS is connected
unsigned long lastGpsByteTime = 0;

// RPM value (update this variable in your own code logic)
int RPM = 0;

// Your 16x16 SD card icon bitmap
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

// 16x16 flashing dot bitmap (a solid filled circle)
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

// SD card presence flag (checked once at boot)
bool sdInserted = false;

// Track if logging occurred in this loop iteration or buffer has data
bool isLogging = false;

// Blink control for logging dot
unsigned long lastBlinkTime = 0;
bool blinkState = false;

// Timezone offset from UTC
const int timezoneOffsetHours = -5;  // e.g. CDT
const int timezoneOffsetMinutes = 0; // partial hour if needed

// --- SD logging buffer ---
const size_t LOG_BUFFER_SIZE = 512;   // Buffer size in bytes
char logBuffer[LOG_BUFFER_SIZE];
size_t logBufferIndex = 0;
unsigned long lastBufferFlushTime = 0;
const unsigned long bufferFlushInterval = 5000; // Flush every 5 seconds

struct LocalTime {
  int hour;
  int minute;
  int second;
};

LocalTime getLocalTime(const TinyGPSDate &date, TinyGPSTime time) {
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

/**
 * Format hour to 12-hour format and return am/pm string
 */
void format12Hour(int hour24, int &hour12, const char* &amPm) {
  if (hour24 == 0) {
    hour12 = 12;
    amPm = "AM";
  } else if (hour24 < 12) {
    hour12 = hour24;
    amPm = "AM";
  } else if (hour24 == 12) {
    hour12 = 12;
    amPm = "PM";
  } else {
    hour12 = hour24 - 12;
    amPm = "PM";
  }
}

/**
 * Generate log file name from date and local time (12h AM/PM)
 */
String generateLogFileName(TinyGPSDate &date, TinyGPSTime time) {
  LocalTime lt = getLocalTime(date, time);
  int hour12;
  const char* amPm;
  format12Hour(lt.hour, hour12, amPm);

  char buf[40];
  snprintf(buf, sizeof(buf), "/%04d%02d%02d_%02d_%02d_%02d%s_log.csv",
    date.year(), date.month(), date.day(),
    hour12, lt.minute, lt.second,
    amPm
  );
  return String(buf);
}

// Current log file name & last date/time used for log file naming
String currentLogFileName = "";
TinyGPSDate lastDate;
TinyGPSTime lastTime;

/**
 * Flush the RAM log buffer to the SD card
 */
void flushLogBuffer() {
  if (logBufferIndex == 0) return; // Nothing to flush

  // Check if date/time changed for filename update
  bool dateChanged = (gps.date.year() != lastDate.year()) ||
                     (gps.date.month() != lastDate.month()) ||
                     (gps.date.day() != lastDate.day());

  bool timeChanged = (gps.time.hour() != lastTime.hour()) ||
                     (gps.time.minute() != lastTime.minute()) ||
                     (gps.time.second() != lastTime.second());

  if (dateChanged || timeChanged || currentLogFileName == "") {
    currentLogFileName = generateLogFileName(gps.date, gps.time);
    lastDate = gps.date;
    lastTime = gps.time;
    Serial.print("Flushing buffer to new file: ");
    Serial.println(currentLogFileName);
  }

  File file = SD.open(currentLogFileName.c_str(), FILE_APPEND);
  if (file) {
    file.write((const uint8_t*)logBuffer, logBufferIndex);
    file.close();
    Serial.print("Flushed ");
    Serial.print(logBufferIndex);
    Serial.println(" bytes to SD");
    logBufferIndex = 0;  // Reset buffer index
    isLogging = false;   // Clear logging flag as buffer is empty
  } else {
    Serial.println("Failed to open log file for writing");
  }
}

/**
 * Buffer GPS and RPM data to RAM buffer
 */
void logToCSV() {
  if (!sdInserted || !gps.location.isValid() || !gps.speed.isValid() || !gps.time.isValid() || !gps.date.isValid()) {
    return;
  }

  // Prepare log line in a temporary buffer
  char line[128];
  int len = snprintf(line, sizeof(line), "%.6f,%.6f,%d,%04d-%02d-%02d %02d:%02d:%02d,%d\n",
      gps.location.lat(),
      gps.location.lng(),
      (int)(gps.speed.mph() + 0.5),
      gps.date.year(),
      gps.date.month(),
      gps.date.day(),
      gps.time.hour(),
      gps.time.minute(),
      gps.time.second(),
      RPM
  );

  if (len < 0 || len >= (int)sizeof(line)) {
    Serial.println("Error formatting log line");
    return;
  }

  // If adding this line would overflow buffer, flush first
  if (logBufferIndex + len >= LOG_BUFFER_SIZE) {
    flushLogBuffer();
  }

  // Copy line into buffer
  memcpy(&logBuffer[logBufferIndex], line, len);
  logBufferIndex += len;

  isLogging = true; // Indicate logging activity
}

void setup() {
  Serial.begin(115200);

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
  } else {
    Serial.println("No SD card found.");
    sdInserted = false;
  }

  // Initialize last buffer flush time
  lastBufferFlushTime = millis();
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
    lastGpsByteTime = millis();
  }

  logToCSV();

  // Flush buffer every 5 seconds if there is data
  if (millis() - lastBufferFlushTime > bufferFlushInterval) {
    flushLogBuffer();
    lastBufferFlushTime = millis();
  }

  // Handle blinking dot timing (500 ms)
  if (millis() - lastBlinkTime > 500) {
    blinkState = !blinkState;
    lastBlinkTime = millis();
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  if (gps.time.isValid() && gps.date.isValid()) {
    LocalTime lt = getLocalTime(gps.date, gps.time);
    int hour12;
    const char* ampm;
    format12Hour(lt.hour, hour12, ampm);
    display.printf("%02d:%02d:%02d %s\n", hour12, lt.minute, lt.second, ampm);
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

  // Draw flashing dot to left of SD icon when logging
  if (sdInserted) {
    if (isLogging && blinkState) {
      display.drawBitmap(83, 0, dot16x16, 16, 16, SH110X_WHITE);
    }
    // Always draw SD card icon solid
    display.drawBitmap(112, 0, sdIcon16x16, 16, 16, SH110X_WHITE);
  }

  display.display();
}
