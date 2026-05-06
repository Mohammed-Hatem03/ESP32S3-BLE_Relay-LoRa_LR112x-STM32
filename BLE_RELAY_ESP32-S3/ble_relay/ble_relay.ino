#include <NimBLEDevice.h>

#define SERVICE_UUID "12E61727-B41A-45D9-A60F-7C3B4E1D9F2A"
#define HEADER_SIZE 41
#define MAX_PACKET_SIZE 250

// Header field offsets
#define OFF_VERSION      0
#define OFF_TYPE         1
#define OFF_MSG_ID       2
#define OFF_SENDER_ID    10
#define OFF_RECEIVER_ID  14
#define OFF_TTL          18
#define OFF_HOP_COUNT    19
#define OFF_TIMESTAMP    20
#define OFF_PAYLOAD_LEN  24
#define OFF_AUTH_TAG     25
#define OFF_PAYLOAD      41

// Packet types
#define TYPE_CHAT  0x01
#define TYPE_ACK   0x02
#define TYPE_HELLO 0x03
#define TYPE_LEAVE 0x04

// Dedup cache
#define CACHE_SIZE      200
#define CACHE_EXPIRY_MS 60000UL

// Relay parameters
#define JITTER_MIN_MS       10
#define JITTER_MAX_MS       50
#define ADVERTISE_DURATION_MS 200

// Debug verbosity
#define PRINT_EVERY_SCAN_RESULT 0   // 1 = log every BLE device seen (very noisy)
#define HEX_DUMP_MAX_BYTES      64

// ── Pre-built UUID object for the hot path ────────────────────────────────────
// Constructed once so onResult() doesn't allocate on every callback.
static NimBLEUUID TARGET_UUID(SERVICE_UUID);

// ─────────────────────────────────────────────────────────────────────────────
// Dedup cache
// ─────────────────────────────────────────────────────────────────────────────
struct CacheEntry {
  uint64_t msgId;
  uint32_t seenAt;
  bool     valid;
};

CacheEntry dedupCache[CACHE_SIZE];
uint16_t   cacheHead  = 0;   // next slot to evict (ring buffer)
uint16_t   cacheCount = 0;

bool isDuplicate(uint64_t msgId) {
  uint32_t now = millis();

  for (uint16_t i = 0; i < cacheCount; i++) {
    CacheEntry& e = dedupCache[i];
    if (!e.valid) continue;

    // Evict stale entries on the fly
    if ((now - e.seenAt) >= CACHE_EXPIRY_MS) {
      e.valid = false;
      if (cacheCount > 0) cacheCount--;
      continue;
    }

    if (e.msgId == msgId) {
      e.seenAt = now;  // refresh timestamp
      return true;
    }
  }

  // Not found — insert at cacheHead (ring buffer, evicts oldest slot)
  dedupCache[cacheHead] = { msgId, now, true };
  cacheHead = (cacheHead + 1) % CACHE_SIZE;
  if (cacheCount < CACHE_SIZE) cacheCount++;
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Relay queue
// ─────────────────────────────────────────────────────────────────────────────
struct RelayPacket {
  uint8_t  data[MAX_PACKET_SIZE];
  uint8_t  len;
  uint32_t advertiseAfter;
};

#define RELAY_QUEUE_SIZE 16
RelayPacket relayQueue[RELAY_QUEUE_SIZE];
uint8_t queueHead   = 0;
uint8_t queueTail   = 0;
uint8_t queueCountQ = 0;

bool enqueueRelay(const uint8_t* data, uint8_t len, uint32_t delayMs) {
  if (queueCountQ >= RELAY_QUEUE_SIZE) {
    Serial.println("[WARN] Relay queue full — dropping oldest packet");
    queueHead = (queueHead + 1) % RELAY_QUEUE_SIZE;
    queueCountQ--;
  }

  RelayPacket& slot = relayQueue[queueTail];
  memcpy(slot.data, data, len);
  slot.len            = len;
  slot.advertiseAfter = millis() + delayMs;

  queueTail = (queueTail + 1) % RELAY_QUEUE_SIZE;
  queueCountQ++;
  return true;
}

bool dequeueRelayPacket(struct RelayPacket* outPkt) {
  if (queueCountQ == 0) return false;
  if (millis() < relayQueue[queueHead].advertiseAfter) return false;

  *outPkt   = relayQueue[queueHead];
  queueHead = (queueHead + 1) % RELAY_QUEUE_SIZE;
  queueCountQ--;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE handles
// ─────────────────────────────────────────────────────────────────────────────
NimBLEScan*          pScan        = nullptr;
NimBLEExtAdvertising* pAdvertising = nullptr;
bool                 isAdvertising = false;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
uint64_t readU64BE(const uint8_t* buf, uint8_t offset) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | buf[offset + i];
  return v;
}

uint32_t readU32BE(const uint8_t* buf, uint8_t offset) {
  return ((uint32_t)buf[offset]     << 24) |
         ((uint32_t)buf[offset + 1] << 16) |
         ((uint32_t)buf[offset + 2] <<  8) |
         ((uint32_t)buf[offset + 3]);
}

const char* typeName(uint8_t t) {
  switch (t) {
    case TYPE_CHAT:  return "CHAT";
    case TYPE_ACK:   return "ACK";
    case TYPE_HELLO: return "HELLO";
    case TYPE_LEAVE: return "LEAVE";
    default:         return "UNKNOWN";
  }
}

void advertisePacket(const uint8_t* data, uint8_t len) {
  if (isAdvertising) {
    pAdvertising->stop(0);
    isAdvertising = false;
  }

  NimBLEExtAdvertisement advData;
  advData.setCompleteServices(TARGET_UUID);
  advData.setServiceData(TARGET_UUID, std::string((const char*)data, len));

  pAdvertising->setInstanceData(0, advData);
  pAdvertising->start(0);
  isAdvertising = true;

  Serial.printf("[ADV] Advertising relay packet len=%d\n", len);
}

// ─────────────────────────────────────────────────────────────────────────────
// Scan callback — UUID filter is the VERY FIRST check
// ─────────────────────────────────────────────────────────────────────────────
class ScanCallback : public NimBLEScanCallbacks {
public:
  void onResult(const NimBLEAdvertisedDevice* device) override {

    // ── GATE 1: must advertise our service UUID ───────────────────────────
    // This is the earliest possible rejection — no memory allocation,
    // no service-data fetch, no parsing. Foreign earbuds, phones, beacons
    // all die here before touching any other code.
    if (!device->isAdvertisingService(TARGET_UUID)) return;

    // ── GATE 2: must carry service data for our UUID ──────────────────────
    std::string svcData = device->getServiceData(TARGET_UUID);
    if (svcData.empty()) return;

    // ── GATE 3: minimum packet length (full header) ───────────────────────
    const uint8_t* raw    = (const uint8_t*)svcData.data();
    uint8_t        rawLen = (uint8_t)svcData.size();

    if (rawLen < HEADER_SIZE) {
      // Still our UUID but malformed — log once and bail
      Serial.printf("[DROP] Packet too short: %u bytes (need %u)\n",
                    rawLen, HEADER_SIZE);
      return;
    }
    if (rawLen > MAX_PACKET_SIZE) {
      Serial.printf("[DROP] Packet too large: %u bytes\n", rawLen);
      return;
    }

    // ── GATE 4: version check ─────────────────────────────────────────────
    uint8_t version = raw[OFF_VERSION];
    if (version != 1) {
      Serial.printf("[DROP] Unknown version: %u\n", version);
      return;
    }

    // ── Parse header fields ───────────────────────────────────────────────
    uint8_t  type       = raw[OFF_TYPE];
    uint64_t msgId      = readU64BE(raw, OFF_MSG_ID);
    uint32_t senderId   = readU32BE(raw, OFF_SENDER_ID);
    uint32_t receiverId = readU32BE(raw, OFF_RECEIVER_ID);
    uint8_t  ttl        = raw[OFF_TTL];
    uint8_t  hopCount   = raw[OFF_HOP_COUNT];
    uint8_t  payloadLen = raw[OFF_PAYLOAD_LEN];

    // ── GATE 5: payload length sanity ────────────────────────────────────
    uint16_t expectedTotal = (uint16_t)HEADER_SIZE + (uint16_t)payloadLen;
    if (rawLen < expectedTotal) {
      Serial.printf("[DROP] payloadLen mismatch: declared=%u actualData=%u\n",
                    payloadLen, rawLen);
      return;
    }

    // ── Log the received packet ───────────────────────────────────────────
    Serial.printf("[RX] type=%s msgId=%08X%08X sender=%08X receiver=%08X"
                  " ttl=%u hop=%u payloadLen=%u rssi=%d\n",
                  typeName(type),
                  (uint32_t)(msgId >> 32), (uint32_t)(msgId & 0xFFFFFFFF),
                  senderId, receiverId, ttl, hopCount, payloadLen,
                  device->getRSSI());

    // ── GATE 6: dedup ─────────────────────────────────────────────────────
    if (isDuplicate(msgId)) {
      Serial.printf("[DEDUP] Drop msgId=%08X%08X\n",
                    (uint32_t)(msgId >> 32), (uint32_t)(msgId & 0xFFFFFFFF));
      return;
    }

    // ── GATE 7: TTL ───────────────────────────────────────────────────────
    if (ttl == 0) {
      Serial.printf("[DROP] TTL=0 msgId=%08X%08X — not relaying\n",
                    (uint32_t)(msgId >> 32), (uint32_t)(msgId & 0xFFFFFFFF));
      return;
    }

    // ── Enqueue for relay ─────────────────────────────────────────────────
    uint8_t relayBuf[MAX_PACKET_SIZE];
    memcpy(relayBuf, raw, rawLen);
    relayBuf[OFF_TTL]       = ttl - 1;
    relayBuf[OFF_HOP_COUNT] = hopCount + 1;

    uint32_t jitter = JITTER_MIN_MS +
                      (esp_random() % (JITTER_MAX_MS - JITTER_MIN_MS + 1));
    enqueueRelay(relayBuf, rawLen, jitter);

    Serial.printf("[RELAY] enqueued msgId=%08X%08X newTtl=%u newHop=%u"
                  " jitter=%ums queue=%u\n",
                  (uint32_t)(msgId >> 32), (uint32_t)(msgId & 0xFFFFFFFF),
                  ttl - 1, hopCount + 1, jitter, queueCountQ);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(921600);   // bumped from 115200 to prevent buffer overflow
  delay(1000);

  Serial.println("\n========================================");
  Serial.println("  Peer Reach BLE Relay Node — ESP32-S3");
  Serial.println("========================================");
  Serial.printf("  Service UUID : %s\n", SERVICE_UUID);
  Serial.println("========================================");

  NimBLEDevice::init("PeerReachRelay");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  pAdvertising = NimBLEDevice::getAdvertising();

  pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new ScanCallback(), false);
  pScan->setActiveScan(false);
#if CONFIG_BT_NIMBLE_EXT_ADV
  pScan->setPhy(NimBLEScan::SCAN_ALL);   // 1M + Coded PHY
#endif
  pScan->setInterval(40);
  pScan->setWindow(40);
  pScan->start(0, false, false);

  Serial.println("[SCAN] Scanner started");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Debug heartbeat — only print when queue is non-zero OR every 5 s when idle
  static uint32_t lastDbg     = 0;
  static uint8_t  lastQueuePrinted = 0;
  uint32_t now = millis();

  bool queueChanged = (queueCountQ != lastQueuePrinted);
  bool heartbeat    = (now - lastDbg >= 5000);   // 5 s idle heartbeat

  if (queueChanged || (heartbeat && queueCountQ > 0)) {
    Serial.printf("[DBG] queueCountQ=%u\n", queueCountQ);
    lastQueuePrinted = queueCountQ;
    lastDbg = now;
  } else if (heartbeat) {
    lastDbg = now;   // reset timer silently when idle
  }

  RelayPacket pkt;
  if (dequeueRelayPacket(&pkt)) {
    Serial.println("[DBG] dequeued packet");

    pScan->stop();

    advertisePacket(pkt.data, pkt.len);
    delay(ADVERTISE_DURATION_MS + 20);

    pAdvertising->stop(0);
    isAdvertising = false;

    pScan->start(0, false, false);
    Serial.printf("[ADV] Done. Resuming scan. Queue depth: %u\n", queueCountQ);
  }

  delay(5);
}
