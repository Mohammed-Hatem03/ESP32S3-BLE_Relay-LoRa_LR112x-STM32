#include <Arduino.h>

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_NSS   10
#define PIN_RESET 9
#define PIN_BUSY  8

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== RAW SPI TEST ===");

  pinMode(PIN_SCK,   OUTPUT);
  pinMode(PIN_MOSI,  OUTPUT);
  pinMode(PIN_MISO,  INPUT);
  pinMode(PIN_NSS,   OUTPUT);
  pinMode(PIN_RESET, OUTPUT);
  pinMode(PIN_BUSY,  INPUT);

  digitalWrite(PIN_NSS, HIGH);
  digitalWrite(PIN_SCK, LOW);

  // Hard reset
  digitalWrite(PIN_RESET, LOW);  delay(10);
  digitalWrite(PIN_RESET, HIGH); delay(500);

  Serial.print("BUSY after reset: ");
  Serial.println(digitalRead(PIN_BUSY) ? "HIGH" : "LOW");

  // Manually clock out GetVersion (0x0101) + 1 dummy + 4 response bytes
  // using bit-bang so we bypass any SPI library issues
  uint8_t cmd[2] = {0x01, 0x01};
  uint8_t dummy  = 0x00;
  uint8_t resp[4] = {0};

  digitalWrite(PIN_NSS, LOW);
  delayMicroseconds(10);

  // Send opcode
  for (int b = 0; b < 2; b++) {
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_MOSI, (cmd[b] >> i) & 1);
      digitalWrite(PIN_SCK, HIGH); delayMicroseconds(2);
      digitalWrite(PIN_SCK, LOW);  delayMicroseconds(2);
    }
  }
  // Send dummy byte
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, 0);
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(2);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(2);
  }
  // Read 4 response bytes
  for (int b = 0; b < 4; b++) {
    resp[b] = 0;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_MOSI, 0);
      digitalWrite(PIN_SCK, HIGH); delayMicroseconds(2);
      int bit = digitalRead(PIN_MISO);
      digitalWrite(PIN_SCK, LOW);  delayMicroseconds(2);
      resp[b] |= (bit << i);
    }
  }

  digitalWrite(PIN_NSS, HIGH);

  Serial.printf("Raw response: 0x%02X 0x%02X 0x%02X 0x%02X\n",
                resp[0], resp[1], resp[2], resp[3]);

  // Also just read MISO raw
  Serial.print("MISO pin raw (idle): ");
  Serial.println(digitalRead(PIN_MISO) ? "HIGH" : "LOW");
}

void loop() {}