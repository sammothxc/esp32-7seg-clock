#include <Arduino.h>
#include <AsyncTCP.h>
#include <EEPROM.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <WiFi.h>

#define DIGIT_COUNT 4
#define EEPROM_SIZE 1024
#define EEPROM_MAGIC 0xA55A1234
#define REFRESH 2200 // microseconds per digit. ~2.2 ms per digit is ~450 Hz refresh for 4 digits
#define COLON_BLINK_INTERVAL 1000 // milliseconds

struct EEPROMstorage {
    uint32_t magic;
    char wifi_ssid[50];
    char wifi_password[50];
    uint8_t use12HourFormat;
    uint8_t DPenabled;
};

const uint8_t segPins[8] = {1,2,3,4,5,6,7,13}; // a,b,c,d,e,f,g,dp (cathode pins, drive LOW to turn on segment)
const uint8_t digitPins[4] = {9,10,11,12}; // anode pins (drive HIGH to enable digit)
uint8_t displayDigits[DIGIT_COUNT];
const uint8_t colon = 8;
const uint8_t led = 48;
const long gmtOffset_sec = -7 * 3600; // MST
const int daylightOffset_sec = 3600; // MDT
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* hostname = "ntp-clock";
const char* pass = "ntp-clock-pass";
const unsigned long NTP_INTERVAL = 3600000UL;
bool colonOn = true;
bool isPM = true;
bool reboot = false;
bool wifiConnected = false;
unsigned long lastColonChange = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastNTPSync = 0;
unsigned long rebootAt = 0;
IPAddress AP_IP(10,1,1,1);
IPAddress AP_subnet(255,255,255,0);
AsyncWebServer server(80);
EEPROMstorage config;

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

void readConf();
void writeConf();
bool connectToWiFi();
void setUpAccessPoint();
void handleWebServerRequest(AsyncWebServerRequest *request);
void startMDNS();
void setUpWebServer();
void wifiBlocker();
void rebootCheck();
void NTPsync();
void updateTime();
void updateColon();
void display();

void setup() {
    Serial.begin(115200);
    Serial.println("Starting up...");
    pinMode(led, OUTPUT);
    digitalWrite(led, HIGH);
    pinMode(colon, OUTPUT);
    digitalWrite(colon, LOW);
    for(uint8_t i=0;i<sizeof(segPins);i++) {
        pinMode(segPins[i], OUTPUT);
        digitalWrite(segPins[i], HIGH);
    }
    for(uint8_t i=0;i<sizeof(digitPins);i++) {
        pinMode(digitPins[i], OUTPUT);
        digitalWrite(digitPins[i], LOW);
    }
    EEPROM.begin(EEPROM_SIZE);
    readConf();
    if(!connectToWiFi()){ setUpAccessPoint(); }
    startMDNS();
    setUpWebServer();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
    NTPsync();
    digitalWrite(led, LOW);
    wifiBlocker();
}

void loop() {
    rebootCheck();
    NTPsync();
    updateTime();
    updateColon();
    display();
}

void readConf() {
    for (size_t i = 0; i < sizeof(EEPROMstorage); i++) {
        ((uint8_t*)&config)[i] = EEPROM.read(i);
    }
    if (config.magic != EEPROM_MAGIC) {
        Serial.println("EEPROM not initialized or corrupt. Loading defaults...");
        memset(&config, 0, sizeof(EEPROMstorage));
        config.magic = EEPROM_MAGIC;
        strcpy(config.wifi_ssid, "your-ssid");
        strcpy(config.wifi_password, "your-password");
        config.use12HourFormat = false;
        config.DPenabled = 1;
        writeConf();
    }
}

void writeConf() {
    for (size_t i = 0; i < sizeof(EEPROMstorage); i++) {
        EEPROM.write(i, ((uint8_t*)&config)[i]);
    }
    EEPROM.commit();
}

bool connectToWiFi() {
    Serial.printf("Connecting to '%s'\n", config.wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    if(WiFi.waitForConnectResult() == WL_CONNECTED) {
        Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
        wifiConnected = true;
        return true;
    } 
    Serial.println("Connection failed");
    return false;
}

void setUpAccessPoint() {
    Serial.println("Setting up AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_subnet);
    WiFi.softAP(hostname, pass);
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void handleWebServerRequest(AsyncWebServerRequest *request) {
    bool save = false;
    if(request->hasParam("ssid",true) && request->hasParam("password",true)) {
        String s = request->getParam("ssid",true)->value();
        String p = request->getParam("password",true)->value();
        s.toCharArray(config.wifi_ssid,sizeof(config.wifi_ssid));
        p.toCharArray(config.wifi_password,sizeof(config.wifi_password));
        writeConf();
        save = true;
    }
    if(request->hasParam("tf", true)) {
        String tf = request->getParam("tf",true)->value();
        config.use12HourFormat = (tf == "12");
        writeConf();
        save = true;
    }
    if (request->method() == HTTP_POST) {
        if (request->hasParam("dp", true)) {
            config.DPenabled = 1;
        } else {
            config.DPenabled = 0;
        }
        writeConf();
        save = true;
    }
    String msg;
    if(save) {
        msg = "<!DOCTYPE html><html><head>";
        msg += "<meta http-equiv='refresh' content='1;url=/' />";
        msg += "<title>Saved! Rebooting...</title></head><body>";
        msg += "<h1>Saved! Rebooting...</h1>";
        msg += "</body></html>";
    } else {
        msg = "<!DOCTYPE html><html><head><title>NTP Clock Config</title></head><body>";
        msg += "<h1>NTP Clock Config</h1><form action='/' method='POST'>";
        msg += "<div>SSID:</div><input type='text' name='ssid' value='"+String(config.wifi_ssid)+"'/>";
        msg += "<div>Password:</div><input type='password' name='password' value='"+String(config.wifi_password)+"'/>";
        msg += "<div>Time Format:</div>";
        msg += "<select name='tf'>";
        msg += config.use12HourFormat ?
                "<option value='24'>24-hour</option><option value='12' selected>12-hour</option>" :
                "<option value='24' selected>24-hour</option><option value='12'>12-hour</option>";
        msg += "</select>";
        msg += "<div>PM Indicator (Decimal Point on 4th digit):</div>";
        msg += "<label>";
        msg += "<input type='checkbox' name='dp' value='1' ";
        msg += (config.DPenabled ? "checked" : "");
        msg += "> Enable";
        msg += "</label>";
        msg += "<div><input type='submit' value='Save'/></div></form>";
        msg += "<h2>Restart ESP</h2>";
        msg += "<form action='/restart' method='POST'>";
        msg += "<input type='submit' value='Restart'/>";
        msg += "</form>";
        msg += "</body></html>";
    }
    request->send(200,"text/html",msg);
    if(save) {
        reboot = true;
        rebootAt = millis();
    }
}

void startMDNS() {
    if(!MDNS.begin(hostname)) {
        Serial.println("Error setting up MDNS responder!");
        return;
    }
    Serial.println("mDNS responder started");
}

void setUpWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ handleWebServerRequest(request); });
    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request){ handleWebServerRequest(request); });
    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta http-equiv='refresh' content='1;url=/' />";
        html += "<title>Restarting...</title></head><body>";
        html += "<h1>Restarting...</h1>";
        html += "</body></html>";
        request->send(200, "text/html", html);
        reboot = true;
        rebootAt = millis();
    });
    ElegantOTA.setAuth("admin", pass);
    ElegantOTA.begin(&server);
    Serial.println("ElegantOTA server started");
    server.begin();
    Serial.println("Web server started");
}

void wifiBlocker() {
    while(!wifiConnected) {
        delay(1000);
        digitalWrite(led, !digitalRead(led));
    }
}

void rebootCheck() {
    if(reboot && millis() >= rebootAt + 2000) {
        server.end();
        delay(500);
        ESP.restart();
    }
}

void NTPsync() {
    if(millis() - lastNTPSync >= NTP_INTERVAL) {
        lastNTPSync = millis();
        unsigned long startAttempt = millis();
        struct tm timeinfo;
        Serial.println("NTP sync requested...");
        if(WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi not connected, reconnecting...");
            WiFi.reconnect();
            delay(500);
            return;
        }
        while (!getLocalTime(&timeinfo)) {
            if(millis() - startAttempt > 3000) {
                Serial.println("NTP sync failed.");
                return;
            }
            delay(100);
        }
        Serial.println("NTP sync complete!");
    }
}

void updateTime() {
    if(millis() - lastTimeUpdate >= 1000) {
        lastTimeUpdate = millis();
        struct tm timeinfo;
        if(getLocalTime(&timeinfo)) {
            int hour = timeinfo.tm_hour;
            int minute = timeinfo.tm_min;

            if(config.use12HourFormat) {
                isPM = (hour >= 12);
                hour = hour % 12;
                if(hour == 0) hour = 12;  // midnight/noon correction
            }
            displayDigits[0] = hour / 10;
            displayDigits[1] = hour % 10;
            displayDigits[2] = minute / 10;
            displayDigits[3] = minute % 10;
        }
    }
}

void updateColon() {
    unsigned long now = millis();
    if (now - lastColonChange >= COLON_BLINK_INTERVAL) {
        colonOn = !colonOn;
        lastColonChange = now;
        digitalWrite(colon, colonOn ? HIGH : LOW);
    }
}

void display() {
    for(size_t pos=0; pos<DIGIT_COUNT; pos++) {
        for(uint8_t i=0;i<sizeof(digitPins);i++) digitalWrite(digitPins[i], LOW);
            uint8_t m = digits[displayDigits[pos]];
            for(uint8_t s=0;s<sizeof(segPins);s++) {
                bool segmentOn = m & (1 << s);
                if (pos == 3 && s == 7 && isPM && config.DPenabled) {
                    segmentOn = true;
                }
                digitalWrite(segPins[s], segmentOn ? LOW : HIGH);
            }
        digitalWrite(digitPins[pos], HIGH);
        delayMicroseconds(REFRESH);
        digitalWrite(digitPins[pos], LOW);
        for(uint8_t s=0;s<sizeof(segPins);s++) digitalWrite(segPins[s], HIGH); // turn segments off to avoid ghosting if switching anodes quickly(?)
    }
}
