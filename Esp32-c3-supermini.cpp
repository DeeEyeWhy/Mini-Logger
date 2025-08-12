/*
  Mini Logger - Full Sketch
  - 8.3-safe filenames: /LYYMMDDxx.CSV  (L = log, YY = year%100, MM = month, DD = day, xx = 00..99)
  - SD hotplug detection
  - Debounced button (short click toggles logging)
  - No "Wait 5 seconds" message; cooldown still prevents rapid toggles but does NOT overwrite bottom message
  - Buffered logging flushed every FLUSH_INTERVAL_SECONDS
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

// Timing for button handling (ms)
#define BUTTON_DEBOUNCE_DELAY 50
#define BUTTON_LONG_PRESS_TIME 1000

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

const int timezoneOffsetHours = -5;
const int timezoneOffsetMinutes = 0;

int RPM = 0; // optional; update externally if you have RPM sensor

unsigned long loggingStartMillis = 0; // <-- fixed declaration

// Button handling
bool buttonJustClicked = false;
bool buttonLongPressed = false;
unsigned long buttonPressStart = 0;
unsigned long buttonLastChange = 0;
bool lastButtonReading = HIGH; // raw last reading
bool buttonStableState = HIGH; // debounced stable state

// Toggle cooldown (prevents very fast toggles). We keep cooldown but do not show "Wait 5 seconds"
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

LocalTime getLocalTime(TinyGPSDate date, TinyGPSTime time) {
  int h = time.hour() + timezoneOffsetHours;
  int m = time.minute() + timezoneOffsetMinutes;
  int s = time.second();
  if (s >= 60) { s -= 60; m += 1; }
  if (s < 0)   { s += 60; m -= 1; }
  if (m >= 60) { m -= 60; h += 1; }
  if (m < 0)   { m += 60; h -= 1; }
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
    // Ensure 8.3-compatible filename and root directory: "/LYYMMDDxx.CSV"
    snprintf(outFilename, outSize, "/L%02d%02d%02d%02d.CSV", yy, mm, dd, i);
    if (!SD.exists(outFilename)) {
      return;
    }
  }
  // fallback if all taken
  snprintf(outFilename, outSize, "/L%02d%02d%02d99.CSV", yy, mm, dd);
}

// open (create) a new log file for writing (returns true on success)
bool openLogFileNew() {
  if (!sdInserted) return false;

  int yy = 0, mm = 0, dd = 0;
  if (gps.date.isValid()) {
    yy = gps.date.year() % 100;
    mm = gps.date.month();
    dd = gps.date.day();
  }
  char fn[20];
  generateNextAvailableLogFileName(fn, sizeof(fn), yy, mm, dd);
  currentLogFileName = String(fn);

  logFile = SD.open(currentLogFileName.c_str(), FILE_WRITE);
  if (!logFile) {
    Serial.print("Failed to create new log file: "); Serial.println(currentLogFileName);
    return false;
  }
  Serial.print("New log file created: "); Serial.println(currentLogFileName);
  // header: single UTC datetime field
  logFile.println("lat,lon,speed_mph,UTC_datetime,RPM");
  logFile.flush();
  return true;
}

// try to reopen the current log file if it's closed
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

  if (!sdInserted) {
    Serial.println("Flush aborted: SD card not inserted");
    bottomMessage = "No SD card!";
    bottomMessageTimestamp = millis();
    return;
  }

  if (!openLogFileIfNeeded()) {
    Serial.println("Flush aborted: file not available");
    bottomMessage = "SD File Error";
    bottomMessageTimestamp = millis();
    return;
  }

  size_t bytesToWrite = logLinesCount * LOG_LINE_SIZE;
  size_t wrote = logFile.write((const uint8_t*)logBuffer, bytesToWrite);

  if (wrote != bytesToWrite) {
    Serial.print("Warning: wrote ");
    Serial.print(wrote);
    Serial.print(" of ");
    Serial.print(bytesToWrite);
    Serial.println(" bytes");

    // Attempt retry: close and reopen and retry write once
    if (logFile) logFile.close();
    if (openLogFileIfNeeded()) {
      size_t retryWrite = logFile.write((const uint8_t*)logBuffer, bytesToWrite);
      if (retryWrite != bytesToWrite) {
        Serial.println("Error: retry write failed, stopping logging");
        isLogging = false;
        bottomMessage = "SD Write Error";
        bottomMessageTimestamp = millis();
      } else {
        Serial.println("Retry write succeeded");
        logFile.flush();
        logLinesCount = 0;

        bottomMessage = "Writing...";
        bottomMessageTimestamp = millis();

        return;
      }
    } else {
      Serial.println("Error: failed to reopen log file");
      isLogging = false;
      bottomMessage = "SD File Error";
      bottomMessageTimestamp = millis();
    }
  } else {
    logFile.flush();
    Serial.print("Flushed ");
    Serial.print(bytesToWrite);
    Serial.println(" bytes to SD");
    logLinesCount = 0;

    bottomMessage = "Writing...";
    bottomMessageTimestamp = millis();
  }
}

// safe buffer a single CSV line (fixed width LOG_LINE_SIZE)
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

  if (logLinesCount < LOG_LINES_MAX) {
    memcpy(&logBuffer[logLinesCount * LOG_LINE_SIZE], line, LOG_LINE_SIZE);
    logLinesCount++;
  } else {
    Serial.println("Warning: log buffer overflow, dropping log line");
  }
}

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplayLogging() {
  unsigned long now = millis();

  if (showFileCreatedMsg) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.print("File created:");
    display.setCursor(0, 12);
    display.print(currentLogFileName);
    display.display();

    if (now - fileCreatedMsgStart >= FILE_CREATED_MSG_DURATION_MS) {
      showFileCreatedMsg = false;
    }
    return; // Skip normal display while showing file created message
  }

  if (now - lastBlinkTime > 500) {
    blinkState = !blinkState;
    lastBlinkTime = now;
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

  bool hasFixLocal = gps.location.isValid() && gps.location.age() < 3000;
  display.printf("Fix: %s\n", hasFixLocal ? "YES" : "NO");

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

  // Show bottom message if any and not expired; moved up a bit to y=100
  if (bottomMessage.length() > 0 && millis() - bottomMessageTimestamp < BOTTOM_MESSAGE_DURATION_MS) {
    display.setCursor(0, 100);
    display.print(bottomMessage);
  }

  display.display();
}

// ==================== BUTTON HANDLING ====================
void handleButton() {
  bool reading = digitalRead(BUTTON_PIN); // HIGH when idle (pullup), LOW when pressed
  unsigned long now = millis();

  // Raw change resets debounce timer
  if (reading != lastButtonReading) {
    buttonLastChange = now;
  }

  // If stable longer than debounce, consider it a stable change
  if ((now - buttonLastChange) > BUTTON_DEBOUNCE_DELAY) {
    if (reading != buttonStableState) {
      // stable state changed
      buttonStableState = reading;
      if (buttonStableState == LOW) {
        // button pressed (stable)
        buttonPressStart = now;
        Serial.println("Button pressed (stable)");
      } else {
        // button released (stable) -> measure duration
        unsigned long pressLen = now - buttonPressStart;
        Serial.print("Button released, len ms = "); Serial.println(pressLen);
        if (pressLen >= BUTTON_LONG_PRESS_TIME) {
          buttonLongPressed = true;
          buttonJustClicked = false;
          Serial.println("Detected long press");
        } else {
          buttonJustClicked = true;
          buttonLongPressed = false;
          Serial.println("Detected short click");
        }
      }
    }
  }

  lastButtonReading = reading;
}

// ==================== GPS FIX CHECK ====================
bool hasFix() {
  return gps.location.isValid() && gps.location.age() < 3000 && gps.satellites.isValid() && gps.satellites.value() >= 3;
}

// ==================== SD Hotplug detection ====================
void checkSDCardPresence() {
  unsigned long now = millis();
  if (now - lastSDCheckMillis < SD_CHECK_INTERVAL_MS) return;
  lastSDCheckMillis = now;

  // SD.begin returns true if card is accessible; calling repeatedly is okay
  bool currentlyInserted = SD.begin(SD_CS);
  if (currentlyInserted && !sdInserted) {
    sdInserted = true;
    Serial.println("SD card inserted.");
    bottomMessage = "SD Inserted";
    bottomMessageTimestamp = now;
  } else if (!currentlyInserted && sdInserted) {
    sdInserted = false;
    Serial.println("SD card removed.");
    bottomMessage = "SD Removed";
    bottomMessageTimestamp = now;

    if (isLogging) {
      if (logLinesCount > 0) flushLogBuffer();
      if (logFile) logFile.close();
      isLogging = false;
    }
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));

  pinMode(BUTTON_PIN, INPUT_PULLUP);

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
    bottomMessage = "SD Ready";
    bottomMessageTimestamp = millis();
  } else {
    Serial.println("No SD card found. Logging disabled.");
    sdInserted = false;
    bottomMessage = "No SD card!";
    bottomMessageTimestamp = millis();
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("No SD card!");
    display.display();
  }

  lastBufferFlushMillis = millis();
  isLogging = false;
  bottomMessage = "";
  bottomMessageTimestamp = 0;

  updateDisplayLogging();
}

// ==================== MAIN LOOP ====================
void loop() {
  handleButton();
  checkSDCardPresence();

  unsigned long now = millis();

  // Handle button click toggle with cooldown
  if (buttonJustClicked) {
    // enforce cooldown but do NOT override the bottom message — we only ignore toggles while cooling
    if ((now - lastToggleMillis) >= TOGGLE_COOLDOWN_MS) {
      // toggle logging
      if (!sdInserted) {
        bottomMessage = "No SD card!";
        bottomMessageTimestamp = now;
      } else {
        if (!isLogging) {
          if (openLogFileNew()) {
            logFile.flush();
            isLogging = true;
            lastLoggedSecond = -1;
            lastBufferFlushMillis = now;
            loggingStartMillis = now;
            lastToggleMillis = now;

            // Show file created confirmation message on OLED for 3 seconds
            showFileCreatedMsg = true;
            fileCreatedMsgStart = now;

            // Suppress normal bottom message "Logging started"
            bottomMessage = "";
            bottomMessageTimestamp = 0;
          } else {
            bottomMessage = "File error!";
            bottomMessageTimestamp = now;
          }
        } else {
          // stop logging
          if (logLinesCount > 0) flushLogBuffer();
          if (logFile) logFile.close();
          isLogging = false;
          lastToggleMillis = now;

          bottomMessage = "Saved as: " + currentLogFileName;
          bottomMessageTimestamp = now;
        }
      }
    } // else cooldown active -> do nothing and keep current bottomMessage
    buttonJustClicked = false; // consume
  }

  // Handle long press if you want (currently unused) — clear flag after showing message
  if (buttonLongPressed) {
    bottomMessage = "Long press";
    bottomMessageTimestamp = now;
    buttonLongPressed = false;
  }

  // Read GPS data
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // Logging once per new second if logging and we have fix & SD present
  if (isLogging && gps.time.isValid() && gps.date.isValid() && hasFix() && sdInserted) {
    int currentSecond = gps.time.second();
    if (currentSecond != lastLoggedSecond) {
      lastLoggedSecond = currentSecond;
      bufferLogLine();
    }
  }

  // Periodic flush
  if (millis() - lastBufferFlushMillis >= BUFFER_FLUSH_INTERVAL_MS) {
    if (logLinesCount > 0 && sdInserted && isLogging) {
      if (!openLogFileIfNeeded()) {
        Serial.println("Failed to open log file at flush time");
      } else {
        flushLogBuffer();
      }
    }
    lastBufferFlushMillis = millis();
  }

  // Update display on interval
  if (now - lastDisplayUpdateMillis > DISPLAY_UPDATE_INTERVAL_MS) {
    updateDisplayLogging();
    lastDisplayUpdateMillis = now;
  }
}
