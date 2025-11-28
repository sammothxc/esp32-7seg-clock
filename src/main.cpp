#include <Arduino.h>
#include <AsyncTCP.h>
#include <EEPROM.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <WiFi.h>

#define EEPROM_SIZE 1024
#define EEPROM_MAGIC 0xA55A1234
#define REFRESH 2200 // microseconds per digit

struct EEPROMstorage {
    uint32_t magic;
    char wifi_ssid[50];
    char wifi_password[50];
    uint8_t use12HourFormat;
};

const long gmtOffset_sec = -7 * 3600; // MST
const int daylightOffset_sec = 3600; // MDT
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* hostname = "ntp-clock";
const char* pass = "ntp-clock-pass";
const uint8_t segPins[7] = {1,2,3,4,5,6,7}; // a,b,c,d,e,f,g (cathode pins, drive LOW to turn ON)
const uint8_t digitPins[4] = {9,10,11,12}; // anode pins (drive HIGH to enable digit)
const uint8_t colon = 8; // colon pin (drive HIGH to turn ON)
const uint8_t led = 48;
const unsigned long NTP_INTERVAL = 3600000UL;
uint8_t displayDigits[4];
bool colonOn = true;
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
    for(uint8_t i=0;i<7;i++){
        pinMode(segPins[i], OUTPUT);
        digitalWrite(segPins[i], HIGH);
    }
    for(uint8_t i=0;i<4;i++){
        pinMode(digitPins[i], OUTPUT);
        digitalWrite(digitPins[i], LOW);
    }
    pinMode(led, OUTPUT);
    pinMode(colon, OUTPUT);
    digitalWrite(colon, LOW);
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
    Serial.print("SoftAP SSID: "); Serial.println(WiFi.softAPSSID());
    Serial.print("SoftAP IP: "); Serial.println(WiFi.softAPIP());
    Serial.print("SoftAP Status: "); Serial.println(WiFi.softAPgetStationNum()); // stations connected

}

void handleWebServerRequest(AsyncWebServerRequest *request) {
    bool save=false;
    if(request->hasParam("ssid",true) && request->hasParam("password",true)){
        String s=request->getParam("ssid",true)->value();
        String p=request->getParam("password",true)->value();
        s.toCharArray(config.wifi_ssid,sizeof(config.wifi_ssid));
        p.toCharArray(config.wifi_password,sizeof(config.wifi_password));
        writeConf(); save=true;
    }
    if(request->hasParam("tf", true)) {
        String tf=request->getParam("tf",true)->value();
        config.use12HourFormat = (tf == "12");
        writeConf(); save=true;
    }
    String msg = "<!DOCTYPE html><html><head><title>NTP Clock Config</title></head><body>";
    if(save){ msg += "<div>Saved! Rebooting...</div>"; } 
    else{
        msg += "<h1>NTP Clock Config</h1><form action='/' method='POST'>";
        msg += "<div>SSID:</div><input type='text' name='ssid' value='"+String(config.wifi_ssid)+"'/>";
        msg += "<div>Password:</div><input type='password' name='password' value='"+String(config.wifi_password)+"'/>";
        msg += "<div>Time Format:</div>";
        msg += "<select name='tf'>";
        msg += config.use12HourFormat ?
                "<option value='24'>24-hour</option><option value='12' selected>12-hour</option>" :
                "<option value='24' selected>24-hour</option><option value='12'>12-hour</option>";
        msg += "</select>";
        msg += "<div><input type='submit' value='Save'/></div></form>";
    }
    msg += "</body></html>";

    request->send(200,"text/html",msg);
    if(save) {
        request->client()->close();
        reboot = true;
        rebootAt = millis();
        return;
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
    ElegantOTA.setAuth("admin", pass);
    ElegantOTA.begin(&server);
    Serial.println("ElegantOTA server started");
    server.begin();
    Serial.println("Web server started");
}

void wifiBlocker() {
    while(!wifiConnected) {
        delay(100);
    }
}

void rebootCheck() {
    if(reboot && millis() >= rebootAt + 1000) {
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
    if (now - lastColonChange >= 1000) {
        colonOn = !colonOn;
        lastColonChange = now;
        digitalWrite(colon, colonOn ? HIGH : LOW);
    }
}

void display() {
    for(uint8_t pos=0; pos<4; pos++) {
        for(uint8_t i=0;i<4;i++) digitalWrite(digitPins[i], LOW);
            uint8_t m = digits[displayDigits[pos]];
            for(uint8_t s=0;s<7;s++) {
                if(m & (1<<s)) digitalWrite(segPins[s], LOW);
                else digitalWrite(segPins[s], HIGH);
            }
        digitalWrite(digitPins[pos], HIGH); // enable digit by driving anode HIGH
        delayMicroseconds(REFRESH); // ~2.2 ms on time per digit, ~450 Hz refresh for 4 digits
        digitalWrite(digitPins[pos], LOW);
        for(uint8_t s=0;s<7;s++) digitalWrite(segPins[s], HIGH); // turn segments off to avoid ghosting if switching anodes quickly (optional)
    }
}
