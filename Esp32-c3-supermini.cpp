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

// ===== SD card presence check function =====
bool isSDInserted() {
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  bool ok = SD.begin(SD_CS, SPI);
  SD.end();
  return ok;
}

// ===== CSV Logging Function =====
void logToCSV() {
  if (!isSDInserted() || !gps.location.isValid() || !gps.speed.isValid() || !gps.time.isValid()) {
    return; // Don't log if conditions aren't met
  }

  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI)) {
    SPI.end();
    return;
  }

  File file = SD.open("/log.csv", FILE_APPEND);
  if (file) {
    file.printf("%.6f,%.6f,%d,%02d:%02d:%02d,%d\n",
      gps.location.lat(),
      gps.location.lng(),
      (int)(gps.speed.mph() + 0.5),
      gps.time.hour(),
      gps.time.minute(),
      gps.time.second(),
      RPM
    );
    file.close();
  }

  SD.end();
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
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
    lastGpsByteTime = millis();
  }

  // Update RPM here, e.g., RPM = readRPM();

  logToCSV();

  display.clearDisplay();
  display.setCursor(0, 0);

  if (gps.time.isValid()) {
    display.printf("%02d:%02d:%02d UTC\n",
      gps.time.hour(),
      gps.time.minute(),
      gps.time.second());
  } else {
    display.println("--:--:--");
  }

  bool gpsConnected = (millis() - lastGpsByteTime < 3000);
  bool hasFix = gps.location.isValid() && gps.location.age() < 3000;

  display.printf("GPS Conn: %s\n", gpsConnected ? "YES" : "NO");
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

  if (isSDInserted()) {
    display.setCursor(110, 0);
    display.print("SD");
  }

  display.display();
}
