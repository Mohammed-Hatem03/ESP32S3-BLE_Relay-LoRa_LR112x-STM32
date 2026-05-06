#include <Arduino.h>
#include <SPI.h>

// -------------------- Pins --------------------
#define PIN_LR_NSS    10
#define PIN_LR_SCK    12
#define PIN_LR_MISO   13
#define PIN_LR_MOSI   11
#define PIN_LR_RESET  9
#define PIN_LR_BUSY   8
#define PIN_LR_DIO1   7

// -------------------- Radio params --------------------
static const uint32_t RF_FREQ_HZ = 868000000;
static const uint8_t LORA_SF = 7;
static const uint8_t LORA_BW = 0x04;
static const uint8_t LORA_CR = 0x01;

// -------------------- Opcodes --------------------
#define OPCODE_SET_STANDBY            0x011C
#define OPCODE_SET_PACKET_TYPE        0x011E
#define OPCODE_SET_RF_FREQUENCY       0x020B
#define OPCODE_SET_MODULATION_PARAMS  0x020F
#define OPCODE_SET_PACKET_PARAMS      0x0210
#define OPCODE_SET_DIO_IRQ_PARAMS     0x0113
#define OPCODE_CLEAR_IRQ_STATUS       0x0114
#define OPCODE_GET_IRQ_STATUS         0x0115
#define OPCODE_WRITE_BUFFER           0x010E
#define OPCODE_READ_BUFFER            0x010F
#define OPCODE_SET_TX                 0x0202
#define OPCODE_SET_RX                 0x0201
#define OPCODE_GET_RX_BUFFER_STATUS   0x0107

#define PACKET_TYPE_LORA              0x01
#define STDBY_RC                      0x00

#define IRQ_TX_DONE                   0x0001
#define IRQ_RX_DONE                   0x0002
#define IRQ_TIMEOUT                   0x0004
#define IRQ_ALL                       0xFFFF

volatile bool dio1Flag = false;
SPIClass spi(FSPI);

// -------------------- FIX 1: BUSY with timeout --------------------
void waitWhileBusy() {
  uint32_t t = millis();
  while (digitalRead(PIN_LR_BUSY) == HIGH) {
    if (millis() - t > 3000) {
      Serial.println("[ERROR] BUSY stuck HIGH — check BUSY pin wiring (GPIO8)");
      break;
    }
    delayMicroseconds(10);
  }
}

void lrSelect()   { digitalWrite(PIN_LR_NSS, LOW); }
void lrDeselect() { digitalWrite(PIN_LR_NSS, HIGH); }

void IRAM_ATTR onDio1Rise() { dio1Flag = true; }

// -------------------- SPI commands --------------------
void lrWriteCommand(uint16_t opcode, const uint8_t* data, uint16_t len) {
  waitWhileBusy();
  lrSelect();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  spi.transfer((opcode >> 8) & 0xFF);
  spi.transfer(opcode & 0xFF);
  for (uint16_t i = 0; i < len; i++) spi.transfer(data[i]);
  spi.endTransaction();
  lrDeselect();
  waitWhileBusy();
}

void lrReadCommand(uint16_t opcode, uint8_t* out, uint16_t len) {
  waitWhileBusy();
  lrSelect();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  spi.transfer((opcode >> 8) & 0xFF);
  spi.transfer(opcode & 0xFF);
  spi.transfer(0x00); // dummy byte
  for (uint16_t i = 0; i < len; i++) out[i] = spi.transfer(0x00);
  spi.endTransaction();
  lrDeselect();
  waitWhileBusy();
}

void lrReset() {
  pinMode(PIN_LR_RESET, OUTPUT);
  digitalWrite(PIN_LR_RESET, LOW);
  delay(10);
  digitalWrite(PIN_LR_RESET, HIGH);
  delay(200);
}

// -------------------- VERSION CHECK --------------------
void lrGetVersion() {
  // Must call this right after reset, before any other command
  // Do NOT call waitWhileBusy() here — chip may not be ready yet
  delay(100);

  lrSelect();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  spi.transfer(0x01); // GetVersion opcode high byte
  spi.transfer(0x01); // GetVersion opcode low byte
  spi.transfer(0x00); // dummy

  uint8_t hw  = spi.transfer(0x00);
  uint8_t use = spi.transfer(0x00);
  uint8_t fw1 = spi.transfer(0x00);
  uint8_t fw2 = spi.transfer(0x00);

  spi.endTransaction();
  lrDeselect();

  Serial.printf("[VERSION] HW=0x%02X  USE=0x%02X  FW=0x%02X%02X\n", hw, use, fw1, fw2);

  if      (hw == 0x22) Serial.println("[VERSION] Chip: LR1110");
  else if (hw == 0x23) Serial.println("[VERSION] Chip: LR1120");
  else if (hw == 0x32) Serial.println("[VERSION] Chip: LR1121");
  else if (hw == 0x00) Serial.println("[VERSION] ERROR: All zeros — MISO not connected or module unpowered");
  else if (hw == 0xFF) Serial.println("[VERSION] ERROR: All ones — SPI not responding, check SCK/MOSI/CS wiring");
  else                 Serial.printf("[VERSION] Unrecognised HW byte 0x%02X\n", hw);
}

// -------------------- Radio config --------------------
void lrSetStandby() {
  uint8_t p = STDBY_RC;
  lrWriteCommand(OPCODE_SET_STANDBY, &p, 1);
}
void lrSetPacketTypeLoRa() {
  uint8_t p = PACKET_TYPE_LORA;
  lrWriteCommand(OPCODE_SET_PACKET_TYPE, &p, 1);
}
void lrSetFrequency(uint32_t freqHz) {
  uint32_t frf = (uint32_t)((double)freqHz * (33554432.0 / 32000000.0));
  uint8_t p[4] = {
    (uint8_t)((frf >> 24) & 0xFF), (uint8_t)((frf >> 16) & 0xFF),
    (uint8_t)((frf >>  8) & 0xFF), (uint8_t)(frf & 0xFF)
  };
  lrWriteCommand(OPCODE_SET_RF_FREQUENCY, p, 4);
}
void lrSetModulation() {
  uint8_t p[4] = { LORA_SF, LORA_BW, LORA_CR, 0x00 };
  lrWriteCommand(OPCODE_SET_MODULATION_PARAMS, p, 4);
}
void lrSetPacketParams(uint8_t payloadLen) {
  uint8_t p[9] = { 0x00, 0x0C, 0x00, payloadLen, 0x01, 0x00, 0x00, 0x00, 0x00 };
  lrWriteCommand(OPCODE_SET_PACKET_PARAMS, p, 9);
}
void lrSetIrqDio1(uint16_t irqMask) {
  uint8_t p[8] = {
    (uint8_t)(irqMask >> 8), (uint8_t)(irqMask & 0xFF),
    (uint8_t)(irqMask >> 8), (uint8_t)(irqMask & 0xFF),
    0x00, 0x00, 0x00, 0x00
  };
  lrWriteCommand(OPCODE_SET_DIO_IRQ_PARAMS, p, 8);
}
uint16_t lrGetIrqStatus() {
  uint8_t r[2] = {0};
  lrReadCommand(OPCODE_GET_IRQ_STATUS, r, 2);
  return (uint16_t)(r[0] << 8) | r[1];
}
void lrClearIrq(uint16_t mask) {
  uint8_t p[2] = { (uint8_t)(mask >> 8), (uint8_t)(mask & 0xFF) };
  lrWriteCommand(OPCODE_CLEAR_IRQ_STATUS, p, 2);
}
void lrWriteBuffer(uint8_t offset, const uint8_t* data, uint8_t len) {
  waitWhileBusy();
  lrSelect();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  spi.transfer((OPCODE_WRITE_BUFFER >> 8) & 0xFF);
  spi.transfer(OPCODE_WRITE_BUFFER & 0xFF);
  spi.transfer(offset);
  for (uint8_t i = 0; i < len; i++) spi.transfer(data[i]);
  spi.endTransaction();
  lrDeselect();
  waitWhileBusy();
}
void lrReadBuffer(uint8_t offset, uint8_t* out, uint8_t len) {
  waitWhileBusy();
  lrSelect();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  spi.transfer((OPCODE_READ_BUFFER >> 8) & 0xFF);
  spi.transfer(OPCODE_READ_BUFFER & 0xFF);
  spi.transfer(offset);
  spi.transfer(0x00); // dummy
  for (uint8_t i = 0; i < len; i++) out[i] = spi.transfer(0x00);
  spi.endTransaction();
  lrDeselect();
  waitWhileBusy();
}
void lrStartTx(uint32_t t = 0) {
  uint8_t p[3] = { (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t };
  lrWriteCommand(OPCODE_SET_TX, p, 3);
}
void lrStartRx(uint32_t t = 0xFFFFFF) {
  uint8_t p[3] = { (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t };
  lrWriteCommand(OPCODE_SET_RX, p, 3);
}
uint8_t lrGetRxPayloadLen() {
  uint8_t r[2] = {0};
  lrReadCommand(OPCODE_GET_RX_BUFFER_STATUS, r, 2);
  return r[0];
}

// -------------------- Radio init --------------------
void radioInit() {
  Serial.println("[INIT] Starting radio init...");
  lrReset();
  lrGetVersion();  // identify chip — check Serial Monitor output
  lrSetStandby();
  lrSetPacketTypeLoRa();
  lrSetFrequency(RF_FREQ_HZ);
  lrSetModulation();
  lrSetPacketParams(16);
  lrSetIrqDio1(IRQ_TX_DONE | IRQ_RX_DONE | IRQ_TIMEOUT);
  lrClearIrq(IRQ_ALL);
  Serial.println("[INIT] Done.");
}

// -------------------- Ping --------------------
void sendPing() {
  const char* msg = "PING";
  uint8_t len = strlen(msg);
  lrSetPacketParams(len);
  lrWriteBuffer(0x00, (const uint8_t*)msg, len);
  lrClearIrq(IRQ_ALL);
  dio1Flag = false;
  Serial.println("[PING] Sending PING...");
  lrStartTx(0);
}

bool waitForPong(uint32_t timeoutMs) {
  lrSetPacketParams(16);
  lrClearIrq(IRQ_ALL);
  dio1Flag = false;
  lrStartRx(0xFFFFFF);

  uint32_t t0 = millis();
  while ((millis() - t0) < timeoutMs) {
    if (dio1Flag) {
      dio1Flag = false;
      uint16_t irq = lrGetIrqStatus();
      lrClearIrq(irq);
      Serial.printf("[PING] IRQ=0x%04X\n", irq);

      if (irq & IRQ_RX_DONE) {
        uint8_t len = lrGetRxPayloadLen();
        uint8_t buf[64] = {0};
        if (len > 63) len = 63;
        lrReadBuffer(0x00, buf, len);
        Serial.printf("[PING] RX <= %s\n", (char*)buf);
        return (strcmp((char*)buf, "PONG") == 0);
      }
      if (irq & IRQ_TIMEOUT) {
        Serial.println("[PING] IRQ: radio timeout");
      }
    }
    delay(1);
  }
  return false;
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("=== LR112x PING NODE ===");

  pinMode(PIN_LR_NSS, OUTPUT);
  digitalWrite(PIN_LR_NSS, HIGH);
  pinMode(PIN_LR_BUSY, INPUT);

  // FIX 2: Pull-down DIO1 so it doesn't float HIGH and spam fake interrupts
  pinMode(PIN_LR_DIO1, INPUT_PULLDOWN);

  // DIAG: Check BUSY state before touching SPI
  Serial.print("[DIAG] BUSY pin at boot: ");
  Serial.println(digitalRead(PIN_LR_BUSY) ? "HIGH (problem — check GPIO8 wiring)" : "LOW (ok)");

  // FIX 3: Use -1 for CS in spi.begin() — we control NSS manually
  spi.begin(PIN_LR_SCK, PIN_LR_MISO, PIN_LR_MOSI, -1);

  attachInterrupt(digitalPinToInterrupt(PIN_LR_DIO1), onDio1Rise, RISING);

  radioInit();
}

// -------------------- Loop --------------------
void loop() {
  sendPing();
  bool ok = waitForPong(2000);
  if (ok) Serial.println("[PING] SUCCESS: got PONG");
  else    Serial.println("[PING] TIMEOUT: no PONG");
  delay(2000);
}
