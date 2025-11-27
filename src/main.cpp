#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <time.h>
#include <EEPROM.h>

struct WifiConf {
  char wifi_ssid[50];
  char wifi_password[50];
  char cstr_terminator = 0;
};

const long  gmtOffset_sec = -7 * 3600; // MST
const int   daylightOffset_sec = 3600; // MDT
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* pass = "ntp-clock-pass";
const char* hostname = "ntp-clock";
IPAddress AP_IP(10,1,1,1);
IPAddress AP_subnet(255,255,255,0);
AsyncWebServer server(80);
WifiConf wifiConf;
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

void readWifiConf();
void writeWifiConf();
bool connectToWiFi();
void setUpAccessPoint();
void handleWebServerRequest(AsyncWebServerRequest *request);
void startMDNS();
void setUpWebServer();
void setSegmentsFor(uint8_t val);
void showDigit(uint8_t pos, uint8_t val);
void updateColon();
void displayTime();
void NTPsync();

void setup() {
    Serial.begin(115200);
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
    EEPROM.begin(512);
    readWifiConf();
    if(!connectToWiFi()){ setUpAccessPoint(); }
    startMDNS();
    setUpWebServer();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
    NTPsync();
}

void loop() {
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
        int rawHour = timeinfo.tm_hour;
        int hour = (rawHour - 7 + 24) % 24;
        int minute = timeinfo.tm_min;

        displayDigits[0] = hour / 10;
        displayDigits[1] = hour % 10;
        displayDigits[2] = minute / 10;
        displayDigits[3] = minute % 10;
    }
}

void NTPsync() {
    unsigned long startAttempt = millis();
    struct tm timeinfo;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
    Serial.println("NTP sync requested...");
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, reconnecting...");
        WiFi.reconnect();
        delay(500);
        return;
    }
    while (!getLocalTime(&timeinfo)) {
        if (millis() - startAttempt > 3000) {
            Serial.println("NTP sync failed.");
            return;  // fail gracefully
        }
        delay(100);
    }
    Serial.println("NTP sync complete!");
}

void readWifiConf() {
    for (size_t i = 0; i < sizeof(wifiConf); i++)
        ((char*)(&wifiConf))[i] = char(EEPROM.read(i));
    wifiConf.cstr_terminator = 0;
}

void writeWifiConf() {
    for (size_t i = 0; i < sizeof(wifiConf); i++)
        EEPROM.write(i, ((char*)(&wifiConf))[i]);
    EEPROM.commit();
}

bool connectToWiFi() {
    Serial.printf("Connecting to '%s'\n", wifiConf.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    WiFi.begin(wifiConf.wifi_ssid, wifiConf.wifi_password);
    if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
        return true;
    } 
    Serial.println("Connection failed");
    return false;
}

void setUpAccessPoint() {
    Serial.println("Setting up AP");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_subnet);
    WiFi.softAP(hostname, pass);
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void handleWebServerRequest(AsyncWebServerRequest *request) {
  bool save=false;
  if(request->hasParam("ssid",true) && request->hasParam("password",true)){
    String s=request->getParam("ssid",true)->value();
    String p=request->getParam("password",true)->value();
    s.toCharArray(wifiConf.wifi_ssid,sizeof(wifiConf.wifi_ssid));
    p.toCharArray(wifiConf.wifi_password,sizeof(wifiConf.wifi_password));
    writeWifiConf(); save=true;
  }

  String msg = "<!DOCTYPE html><html><head><title>ESP Wifi Config</title></head><body>";
  if(save){ msg += "<div>Saved! Rebooting...</div>"; } 
  else{
    msg += "<h1>ESP Wifi Config</h1><form action='/wifi' method='POST'>";
    msg += "<div>SSID:</div><input type='text' name='ssid' value='"+String(wifiConf.wifi_ssid)+"'/>";
    msg += "<div>Password:</div><input type='password' name='password' value='"+String(wifiConf.wifi_password)+"'/>";
    msg += "<div><input type='submit' value='Save'/></div></form>";
  }
  msg += "</body></html>";

  request->send(200,"text/html",msg);
  if(save){ request->client()->close(); delay(1000); ESP.restart(); }
}

void setUpWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Hi! This is NTP Clock :)");
    });
    server.on("/wifi", HTTP_GET, handleWebServerRequest);
    server.on("/wifi", HTTP_POST, handleWebServerRequest);
    ElegantOTA.setAuth("admin", pass);
    ElegantOTA.begin(&server);
    Serial.println("ElegantOTA server started");
    server.begin();
    Serial.println("Web server started");
}

void startMDNS() {
    if (!MDNS.begin(hostname)) { //http://ntp-clock.local
        Serial.println("Error setting up MDNS responder!");
        while (1) {
        delay(1000);
        }
    }
    Serial.println("mDNS responder started");
}