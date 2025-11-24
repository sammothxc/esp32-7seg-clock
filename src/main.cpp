#include <Arduino.h>

const uint8_t segPins[7] = {2,3,4,5,6,7,8}; // a,b,c,d,e,f,g (cathode pins, drive LOW to turn ON)
const uint8_t digitPins[4] = {9,10,11,12};   // anode pins (drive HIGH to enable digit)
const uint8_t colon = 9;  // colon pin (drive HIGH to turn ON)

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

uint8_t displayDigits[4] = {1,2,3,4}; // shows 12:34
bool colonState = true;
unsigned long lastBlink = 0;

void setup() {
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

void loop() {
  if (millis() - lastBlink >= 500) {
    lastBlink = millis();
    colonState = !colonState;
    digitalWrite(colon, colonState ? HIGH : LOW);
  }

  for (uint8_t pos=0; pos<4; pos++){
    showDigit(pos, displayDigits[pos]);
  }
}