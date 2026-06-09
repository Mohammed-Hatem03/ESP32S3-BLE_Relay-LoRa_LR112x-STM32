# Peer Reach — ESP32-S3/LoRa Hardware Engineering Journal
## Repository: ESP32S3-BLE_Relay-LoRa_LR112x-STM32

This document records the hardware decisions, failure modes, and engineering
outcomes for the embedded relay and LoRa gateway components of the Peer Reach
mesh messaging system. It is intended as a living engineering log for the thesis
evaluation chapter and for anyone reproducing this hardware setup.

---

## 1. Initial Hardware Plan — STM32 L476RG + LR1120

The original hardware selection used the **STM32 Nucleo-L476RG** as the host
MCU for the **Semtech LR1120DVK1TBKS** LoRa development kit. The two boards
were chosen together because the LR1120 shield uses an Arduino/Mbed-compatible
32-pin shield connector that plugs directly onto the Nucleo-64 form factor — a
physical fit confirmed by the kit's design intent.

The plan was:
- STM32 L476RG hosts the LR1120 over SPI via the shield connector
- STM32 firmware handles LoRa packet forwarding between BLE clusters
- Android phones communicate over BLE; the STM32 bridges to LoRa

### The Blocking Problem: No Bluetooth on the STM32

After beginning firmware development, it was confirmed that the STM32 L476RG
has **no wireless radio of any kind** — no BLE or Wi-Fi, and no integrated
transceiver. It is a pure microcontroller.

The Peer Reach BLE stack requires:
- **BLE 5.0** with Extended Advertising (AUX_ADV_IND PDUs)
- **LE Coded PHY** scanning (125 kbps, S=8) to match Android phone advertising
- Simultaneous scan + advertise for relay operation

None of these are achievable on the STM32 L476RG without an external radio module.

---

## 2. Options Evaluated

### Option A — ESP32 as BLE Coprocessor Alongside STM32

The ESP32-S3 handles BLE; the STM32 handles the LR1120 over SPI. They exchange
packets over UART.

**Why rejected:** Two MCUs introduce an inter-processor protocol that duplicates
the complexity of the mesh protocol already being built. Every packet crosses two
MCU boundaries. Debug sessions become ambiguous — failures could originate in
either chip or in the UART link between them. Two power rails, two firmware
binaries, two flash targets. The integration risk exceeds the benefit for a
prototype evaluation system.

### Option B — Dedicated BLE 5 Module on STM32

Add a BLE 5 module (e.g. u-blox NINA-B4) to the STM32 over UART/SPI.

**Why rejected:** Most BLE 5 modules expose AT command interfaces oriented around
GATT profiles, not raw Extended Advertising PDU control. Peer Reach uses
connectionless advertisement-based flooding — there is no GATT session. Finding a
module with raw HCI access that supports LE Extended Advertising in non-connectable
non-scannable mode is difficult to source and requires writing an HCI driver on the
STM32 side. The integration effort is comparable to Option A.

### Option C — Replace STM32 with ESP32-S3 (Selected)

Use the **ESP32-S3** as the sole MCU for both the BLE stack and the LR1120 LoRa
interface. The ESP32-S3 drives the LR1120 over SPI directly; BLE and LoRa stacks
run as concurrent FreeRTOS tasks on the same chip.

**Why selected:**
- ESP32-S3 has native BLE 5.0 with Extended Advertising and LE Coded PHY
- Single chip, single toolchain (Arduino/ESP-IDF), single debug target
- LR1120 SPI interface maps cleanly to ESP32-S3 FSPI peripheral
- NimBLE stack on ESP32-S3 supports `CONFIG_BT_NIMBLE_EXT_ADV=1` for extended
  advertising and extended scanning — required to interoperate with Android phones

**What happens to the STM32:** The STM32 L476RG is retained as a wired packet
injector for stress testing via UART, and is documented as hardware that was
evaluated and replaced due to the BLE 5 Coded PHY requirement.

---

## 3. ESP32-S3 + LR1120 Wiring and Pin Assignment

The LR1120DVK1TBKS shield connector is designed for the Nucleo-64 Arduino header
form factor and does not physically mate with ESP32-S3 DevKit boards. The
connection is made via **fly wires (female-to-male jumper cables)** from the
shield's Arduino header pins to the ESP32-S3 GPIO header.

### Pin Mapping

| Signal   | LR1120 Shield Header | ESP32-S3 GPIO |
|----------|----------------------|---------------|
| SCK      | D13 (Arduino SPI)    | GPIO 12       |
| MISO     | D12 (Arduino SPI)    | GPIO 13       |
| MOSI     | D11 (Arduino SPI)    | GPIO 11       |
| NSS (CS) | D10                  | GPIO 10       |
| BUSY     | D3                   | GPIO 8        |
| DIO1     | D5                   | GPIO 7        |
| NRESET   | D6                   | GPIO 9        |
| 3.3V     | 3V3 rail             | 3V3 pin       |
| GND      | GND                  | GND           |

SPI is instantiated as `SPIClass spi(FSPI)` with `spi.begin(SCK, MISO, MOSI, -1)`.
The NSS pin is driven manually (`digitalWrite`) rather than delegated to the SPI
peripheral, which is standard practice for LR11xx devices because NSS must remain
stable across the BUSY polling window.

### Power Decoupling

A **100 µF electrolytic capacitor** is placed across the 3.3V and GND rails on
the breadboard at the LR1120 power pins. The LR1120 draws approximately 100 mA
peak during LoRa TX. Without decoupling, the current spike can cause the ESP32-S3
onboard 3.3V LDO to momentarily sag, resetting the chip mid-transmission.

---

## 4. LR1120 SPI Bring-Up — The MISO Problem

The first firmware written for the LoRa node was `pins_lora_check.ino`, a
**raw bit-bang SPI diagnostic** that bypasses the Arduino SPI library entirely.
It manually clocks out the LR1120 `GetVersion` command (opcode `0x0101`) and
reads back 4 response bytes.

### Problem Encountered

After wiring and initial power-on, the MISO line read **0x00 on every byte** when
using the SPI library. The version response was `0x00 0x00 0x00 0x00`, which the
LR1120 datasheet identifies as "MISO not connected or module unpowered."

The bit-bang sketch confirmed the same result, ruling out SPI library configuration
as the cause. Root causes investigated:

1. **MISO wire not seated in the shield header** — the female jumper connector
   was not fully inserted into the shield's Arduino pin socket. The pin made
   intermittent contact but read floating LOW. Fix: reseated firmly.

2. **SPI CS passed to `spi.begin()`** — the original code passed `PIN_NSS` as
   the CS argument to `spi.begin()`, which caused the ESP32 SPI peripheral to
   also assert the pin, conflicting with manual GPIO control. Fix: pass `-1` as
   the CS argument and control NSS exclusively via `digitalWrite`.

3. **BUSY pin floating HIGH at boot** — on the first reset, the BUSY line was
   read as HIGH before the LR1120 had completed its internal boot sequence. The
   `waitWhileBusy()` function correctly detects this but without a timeout it
   would hang indefinitely. Fix: added a 3-second timeout with a diagnostic
   message: `[ERROR] BUSY stuck HIGH — check BUSY pin wiring`.

4. **DIO1 floating and triggering spurious interrupts** — DIO1 was left as a
   plain `INPUT` and floated HIGH at rest, causing the interrupt handler
   `onDio1Rise()` to fire continuously before any LoRa packet arrived. Fix:
   changed to `INPUT_PULLDOWN`.

After resolving these issues, `GetVersion` returned `HW=0x23`, confirming the
chip is an **LR1120** as expected.

### Diagnostic Output After Fix

```
=== RAW SPI TEST ===
BUSY after reset: LOW (ok)
Raw response: 0x23 0x01 0x04 0x02
MISO pin raw (idle): LOW
```

---

## 5. LR1120 Ping Node — LoRa Link Verification

With SPI communication confirmed, `lr112x_ping_node.ino` implements a LoRa
ping/pong test to verify the full radio path:

- **Frequency:** 868 MHz
- **Modulation:** LoRa SF7, BW 500 kHz (0x04), CR 4/5
- **Packet format:** Raw ASCII strings — `"PING"` TX, `"PONG"` expected RX
- **Timeout:** 2 seconds per ping cycle

The firmware initialises the radio via direct SPI opcodes matching the LR1120
command set (16-bit opcodes, as distinct from the SX126x 8-bit opcode format).
Key opcodes used:

| Opcode   | Function               |
|----------|------------------------|
| `0x011C` | SetStandby             |
| `0x011E` | SetPacketType (LoRa)   |
| `0x020B` | SetRfFrequency         |
| `0x020F` | SetModulationParams    |
| `0x0210` | SetPacketParams        |
| `0x0113` | SetDioIrqParams        |
| `0x0202` | SetTx                  |
| `0x0201` | SetRx (continuous)     |
| `0x010F` | ReadBuffer             |
| `0x010E` | WriteBuffer            |

This firmware is used for hardware bring-up verification only. Integration with
the BLE relay pipeline (Phase 11) will replace the ping payload with forwarded
`MeshPacket` bytes.

---

## 6. BLE Pure Relay Node — Firmware and Results

The `ble_relay.ino` firmware implements a **connectionless BLE relay** using the
NimBLE stack. No LoRa hardware is present on this node variant. It is deployed on
two standalone ESP32-S3 boards as intermediate BLE hops in the mesh.

### Architecture

The relay operates in a scan → queue → advertise cycle:

1. **Scan** continuously for BLE Extended Advertising PDUs containing the Peer
   Reach service UUID (`12E61727-B41A-45D9-A60F-7C3B4E1D9F2A`)
2. **Validate** received packets against the 7-gate pipeline (UUID → service data
   present → minimum length → version byte → payload length sanity → dedup cache
   → TTL)
3. **Mutate** the packet: decrement TTL by 1, increment hop_count by 1
4. **Enqueue** with a random jitter delay (10–50 ms) to prevent collision storms
5. **Stop scan**, **advertise** the mutated packet for 220 ms, **resume scan**

### Extended Advertising Configuration

The build flag `CONFIG_BT_NIMBLE_EXT_ADV=1` (set in `build_opt.h`) enables
NimBLE's extended advertising and extended scanning support. Without this flag,
NimBLE defaults to legacy BLE 4.x advertising which cannot:
- Transmit packets larger than 31 bytes
- Receive Android Extended Advertising PDUs (`AUX_ADV_IND`)

The scan PHY is set to `NimBLEScan::SCAN_ALL` which enables scanning on both
1M PHY and Coded PHY simultaneously, matching the Android phones' advertised
PHY configuration.

Advertising uses `NimBLEExtAdvertising` with `NimBLEExtAdvertisement`, placing
the packet bytes in the service data field keyed by the service UUID — matching
the exact format produced by the Android `BleAdvertiser.kt`.

### Dedup Cache

A ring-buffer cache of 200 entries with 60-second expiry prevents relay loops.
Entries are evicted lazily on access (stale entries with `valid=false` are skipped
and their slot is recycled). Cache head and count use `uint16_t` to support
future expansion beyond 255 entries without silent overflow.

### Verified Results

The relay node was tested in a three-device configuration:

- **Phone A** (Samsung SM-G975F, sender) and **Phone B** (Samsung SM-T505N,
  receiver) placed out of direct BLE range
- **ESP32-S3 relay** placed between them

Serial monitor confirmed the full relay pipeline firing:

```
[RX] type=CHAT msgId=AABBCCDD11223344 sender=DEADBEEF receiver=CAFEBABE
     ttl=5 hop=0 payloadLen=42 rssi=-61
[RELAY] enqueued msgId=AABBCCDD11223344 newTtl=4 newHop=1 jitter=33ms queue=1
[DBG] dequeued packet
[ADV] Advertising relay packet len=83
[ADV] Done. Resuming scan. Queue depth: 0
```

Phone B received the message successfully. End-to-end delivery confirmed across
a relay hop. MDR = 100% in controlled desk-distance tests.

---

## 7. Repository File Index

| File                    | Purpose                                              |
|-------------------------|------------------------------------------------------|
| `ble_relay.ino`         | ESP32-S3 standalone BLE relay firmware               |
| `lr112x_ping_node.ino`  | LR1120 LoRa ping/pong bring-up test                  |
| `pins_lora_check.ino`   | Raw bit-bang SPI diagnostic for LR1120 bring-up      |    |
| `README.md`             | This document                                        |

---

## 8. Known Issues and Open Items

- **LoRa ↔ BLE bridge firmware (Phase 11)** is not yet implemented. The
  `lr112x_ping_node.ino` and `ble_relay.ino` firmwares are separate; combining
  them into a single gateway firmware with concurrent FreeRTOS tasks is the next
  development milestone.

- **LR1120 antenna:** An 868 MHz whip antenna (SMA) must be connected before
  powering the LoRa module. Operating the LR1120 without an antenna during TX
  causes reflected power that degrades the PA over time.

- **Fly-wire mechanical stability:** The female-to-male jumper wires between the
  LR1120 shield and ESP32-S3 DevKit are adequate for lab testing but fragile for
  field evaluation. A custom adapter PCB or soldered breadboard is recommended
  before the 500 m outdoor test.

- **Duty cycle:** No LoRa duty-cycle limiting is implemented in the current
  firmware. Egypt has no formal LoRa duty-cycle regulation, but the 1% EU limit
  (868 MHz band) is a useful design reference for airtime budgeting in the thesis
  evaluation.
