#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <time.h>
#include "secrets.h"

const char* ssid = SECRET_SSID2_4;
const char* password = SECRET_SSID_PASS;
const char* hostname = "NTPClock";
IPAddress AP_IP(10,1,1,1);
IPAddress AP_subnet(255,255,255,0);

struct WifiConf {
  char wifi_ssid[50];
  char wifi_password[50];
  char cstr_terminator = 0;
};

const uint8_t segPins[7] = {1,2,3,4,5,6,7}; // a,b,c,d,e,f,g (cathode pins, drive LOW to turn ON)
const uint8_t digitPins[4] = {9,10,11,12}; // anode pins (drive HIGH to enable digit)
const uint8_t colon = 8; // colon pin (drive HIGH to turn ON)
const uint8_t button1 = 13;
const uint8_t button2 = 14;
const uint8_t led = 48; // onboard
bool colonOn = true;
unsigned long lastColonChange = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastNTPSync = 0;
const unsigned long NTP_INTERVAL = 3600000UL;
uint8_t displayDigits[4] = {8,8,8,8}; // 88:88 digit test

// bits 0..6 = a..g
const uint8_t digits[10] = {
    0b00111111, //0
    0b00000110, //1
    0b01011011, //2
    0b01001111, //3
    0b01100110, //4
    0b01101101, //5
    0b01111101, //6
    0b00000111, //7
    0b01111111, //8
    0b01101111  //9
};

void setSegmentsFor(uint8_t val);
void showDigit(uint8_t pos, uint8_t val);
void updateColon();
void displayTime();
void NTPsync();

void setup() {
    Serial.begin(9600);
    Serial.println("Starting up...");
    for (uint8_t i=0;i<7;i++){
        pinMode(segPins[i], OUTPUT);
        digitalWrite(segPins[i], HIGH);
    }
    for (uint8_t i=0;i<4;i++){
        pinMode(digitPins[i], OUTPUT);
        digitalWrite(digitPins[i], LOW);
    }
    pinMode(colon, OUTPUT);
    digitalWrite(colon, LOW);
    pinMode(led, OUTPUT);
    digitalWrite(led, LOW);
    Serial.println("Connecting to WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA
    .onStart([]() {
        Serial.println("OTA Update Started");
    })
    .onEnd([]() {
        Serial.println("\nOTA Update Finished");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]\n", error);
    });

    ArduinoOTA.begin();
    Serial.println("OTA Ready");
    setenv("TZ", "MST7MDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("Waiting for time...");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo)) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nTime synchronized!");
}

void loop() {
    ArduinoOTA.handle();
    updateColon();

    if (millis() - lastTimeUpdate >= 1000) {
        lastTimeUpdate = millis();
        displayTime();
    }

    for (uint8_t pos=0; pos<4; pos++){
        showDigit(pos, displayDigits[pos]);
    }

    if (millis() - lastNTPSync >= NTP_INTERVAL) {
        lastNTPSync = millis();
        NTPsync();
    }
}

void setSegmentsFor(uint8_t val) {
    uint8_t m = digits[val];
    for (uint8_t s=0;s<7;s++){
        if (m & (1<<s)) digitalWrite(segPins[s], LOW);
        else digitalWrite(segPins[s], HIGH);
    }
}

void showDigit(uint8_t pos, uint8_t val) {
    for (uint8_t i=0;i<4;i++) digitalWrite(digitPins[i], LOW); // ensure digits off
    setSegmentsFor(val); // set segments for value
    digitalWrite(digitPins[pos], HIGH); // enable digit by driving anode HIGH
    delayMicroseconds(2200); // ~2.2 ms on time per digit, ~450 Hz refresh for 4 digits
    digitalWrite(digitPins[pos], LOW); // turn off
    for (uint8_t s=0;s<7;s++) digitalWrite(segPins[s], HIGH); // turn segments off to avoid ghosting if switching anodes quickly (optional)
}

void updateColon() {
    unsigned long now = millis();
    if (colonOn && now - lastColonChange >= 1000) {
        colonOn = false;
        lastColonChange = now;
        digitalWrite(colon, LOW);
    }
    else if (!colonOn && now - lastColonChange >= 1000) {
        colonOn = true;
        lastColonChange = now;
        digitalWrite(colon, HIGH);
    }
}

void displayTime() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        int hour = timeinfo.tm_hour;
        int minute = timeinfo.tm_min;

        displayDigits[0] = hour / 10;
        displayDigits[1] = hour % 10;
        displayDigits[2] = minute / 10;
        displayDigits[3] = minute % 10;
    }
}

void NTPsync() {
    Serial.println("NTP sync requested...");

    // Make sure WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, reconnecting...");
        WiFi.reconnect();
        delay(500);
        return;
    }

    // Request new time from NTP servers
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;
    unsigned long startAttempt = millis();

    // Try up to 3 seconds to get a valid time
    while (!getLocalTime(&timeinfo)) {
        if (millis() - startAttempt > 3000) {
            Serial.println("NTP sync failed.");
            return;  // fail gracefully
        }
        delay(100);
    }

    Serial.println("NTP sync complete!");
}
