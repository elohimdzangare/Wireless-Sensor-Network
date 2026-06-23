# ESP32 Wireless Sensor Network

A wireless environmental monitoring system using battery powered ESP32 sensor nodes that measure temperature, humidity and light across three locations. A central gateway collects everything and sends it to MATLAB for live plotting and logging.

<img width="750" height="500" alt="full-sensor-network" src="https://github.com/elohimdzangare/Wireless-Sensor-Network/blob/main/images/Full%20Network.jpg" />

**Status:** Core system is built and working end to end (nodes to gateway to MATLAB). Currently running longer test sessions to collect real data.

---

## Overview

Three sensor nodes (Room A, B, C) each have a DHT22 and a BH1750 sensor and run off 4x AA batteries. Each node wakes up from deep sleep, takes a reading, sends it to the gateway over ESP-NOW, tells the gateway how long it's about to sleep for, then powers down again. The gateway stays on all the time, keeps track of all three nodes, manages 8 status LEDs, and forwards everything to MATLAB over TCP so it can be logged and plotted live.

Rooms are just labelled A, B and C for now instead of actual room names, so the sensors can be moved around without having to change any code. A separate note of what each letter actually corresponds to is kept somewhere.

---

## Repo Structure

```
/sensor-network
├── README.md
├── /gateway
│   └── gateway_matlab.ino       (final gateway sketch)
├── /node
│   └── sensor_node_final.ino    (final sensor node sketch, used on all 3 nodes)
├── /matlab
│   └── live_monitor.m
└── /archive                     (earlier test sketches, kept for reference)
    ├── mac_address.ino
    ├── led_esp_test.ino
    ├── sensor_test.ino
    ├── sensor_node_test.ino
    ├── gateway_receive_test.ino
    ├── gateway_receive_final.ino
    └── hotspot_connection_test.ino
```

The `/archive` folder is just earlier versions and one-off test sketches from along the way (checking the MAC address, testing the LEDs on their own, testing the sensors before adding ESP-NOW, etc). Not needed to actually run the project, just there to show how it was built up step by step.

---

## Hardware

| Component | Quantity | Notes |
|-----------|----------|-------|
| ESP32 (regular size) | 4 | 3 sensor nodes + 1 gateway |
| DHT22 (AZDelivery) | 3 | Temperature and humidity, one per node |
| BH1750FVI (ARCELI) | 3 | Light level in lux, one per node |
| 4x AA batteries | 3 sets | One set per sensor node, into VIN |
| LEDs | 8 | All status LEDs are on the gateway |
| Resistors | 8 | 470Ω, used for all LEDs regardless of colour |

> **Sensor power:** both sensors on every node need their VCC wired to the ESP32's 3.3V pin, not VIN. VIN is unregulated, so it just passes whatever voltage is feeding it straight through. Wiring a sensor to VIN instead of 3.3V caused issues, more on that in the Debugging section below.

> **Gateway power:** currently running off USB during testing (power bank or laptop). The 5V pin from USB powers the LED circuit. All ESP32 ground pins are tied to a shared ground.

---

## Network Architecture

```
[Node: Room A]  DHT22 + BH1750  ──┐
[Node: Room B]  DHT22 + BH1750  ──┼──(ESP-NOW)──> [Gateway ESP32] ──(TCP, port 8080)──> [MATLAB]
[Node: Room C]  DHT22 + BH1750  ──┘
   4x AA, deep sleep                              connected to WiFi, always on
```

- **Nodes to gateway:** ESP-NOW. Deep sleep wipes the chip's RAM every time it wakes up, so reconnecting to WiFi from scratch each cycle would take 1 to 3 seconds and use a lot of current for a battery powered node. ESP-NOW skips all that, the node just re-registers the gateway as a peer and sends straight away, no router needed.
- **Gateway to MATLAB:** TCP over WiFi. Only the gateway needs an actual WiFi connection since it's the only one that's always awake, so it only pays that startup cost once.
- **WiFi channel:** ESP-NOW and the gateway's WiFi connection use the same radio, so they need to be on the same channel. The nodes default to channel 1 since they never actually connect to WiFi. So far the gateway has landed on channel 1 both times it connected, once to a phone hotspot and once to home WiFi, so this hasn't been an issue yet. If it ever lands on a different channel, ESP-NOW between the nodes and gateway would stop working without much warning.

---

## Sensor Node Behaviour

Every wake cycle goes through `setup()` from scratch, since deep sleep wipes RAM and there's no way to "resume" where you left off:

1. Wake up from deep sleep (timer based)
2. Set up sensors, with a short delay to let them stabilise
3. Set up WiFi station mode and ESP-NOW, re-register the gateway as a peer
4. Read the DHT22 and BH1750
5. Build a `MSG_DATA` packet and send it to the gateway, including flags for whether each sensor read worked
6. Send a `MSG_SLEEP` packet saying how long it's about to sleep for
7. Set the wake up timer and go into deep sleep

Each node currently sleeps for **360 seconds (6 minutes)**, which works out to 10 readings an hour. Each node tells the gateway its own sleep duration in the sleep message, so if you ever want to change one node's interval you don't need to touch the gateway code at all.

> **Sensor settling time:** right after waking up, the sensors need a moment before their first reading can be trusted, especially the DHT22 which often fails its very first read after power up. The node code adds about a 2 second delay before reading, and retries the DHT22 once if the first attempt comes back invalid.

### Data Packet Structure

```cpp
typedef struct {
  uint8_t  node_id;             // 1 = Room A, 2 = Room B, 3 = Room C
  uint8_t  msg_type;            // 0 = MSG_DATA, 1 = MSG_SLEEP
  float    temperature;         // in °C, -999 if DHT22 failed
  float    humidity;            // in %, -999 if DHT22 failed
  float    lux;                 // -999 if BH1750 failed
  bool     dht_ok;
  bool     bh1750_ok;
  uint32_t sleep_duration_sec;  // only matters when msg_type == MSG_SLEEP
} SensorPacket;
```

A node always sends a data packet every cycle, even if a sensor failed. The idea is that whether a node can be reached and whether its sensors are actually working should be tracked separately, so a broken sensor doesn't make the whole node look offline. If a reading fails it just gets sent as -999 with the matching `_ok` flag set to false.

---

## Gateway Behaviour

- Runs a boot sequence with all the LEDs as a quick self test, then connects to WiFi and starts a TCP server on port 8080
- Stays on all the time, accepts a MATLAB connection whenever one comes in
- Handles incoming ESP-NOW packets based on `msg_type`:
  - `MSG_SLEEP` works out when that node should next wake up (current time plus the sleep duration it gave plus a grace window) and turns that node's awake LED off
  - `MSG_DATA` flashes the RX LED, turns the awake LED back on, updates whether that node has a sensor fault, clears any "unreachable" flag, and forwards the data to MATLAB
- Keeps checking if any sleeping node has gone past its expected wake up time, and flags it as unreachable if so
- Sends `STATUS:`, `ERROR:` and `DATA:` messages to Serial and to MATLAB at the same time
- Flashes the TX LED every time something gets sent to MATLAB

---

## Gateway LEDs

| LED | Colour | What it does |
|-----|--------|---------------|
| Room A RX | Blue | Flashes for 100ms when a packet comes in from Room A |
| Room A Awake/Asleep | White | On while Room A is awake, off once it announces it's sleeping |
| Room B RX | Orange | Flashes for 100ms when a packet comes in from Room B |
| Room B Awake/Asleep | White | On while Room B is awake, off once it announces it's sleeping |
| Room C RX | Yellow | Flashes for 100ms when a packet comes in from Room C |
| Room C Awake/Asleep | White | On while Room C is awake, off once it announces it's sleeping |
| Data Sent (TX to MATLAB) | Green | Flashes for 100ms whenever a message goes out to MATLAB |
| Error | Red | See the error states below |

All the LED flashing is non-blocking. The usual way to blink an LED is `digitalWrite(HIGH)`, then `delay(100)`, then `digitalWrite(LOW)`, but `delay()` freezes the whole chip for that time, including the part that listens for incoming ESP-NOW packets. So instead, the gateway just remembers what time an LED turned on, and checks on every loop whether 100ms has passed yet to turn it back off. Same blink, but the gateway never actually stops and can't miss a packet while waiting.

### Error LED States

| State | Behaviour | What it means |
|-------|-----------|----------------|
| `ERROR_NONE` | Off | Everything's fine |
| `ERROR_SENSOR_FAULT` | Slow blink, 500ms on/off | A node is still reachable but one of its sensors failed |
| `ERROR_NODE_UNREACHABLE` | Solid on | A node didn't wake up in time, missed its expected window |

If both are happening at once, unreachable wins and the LED goes solid. The thinking is a node going completely silent is worse than one sensor on one node acting up.

### Gateway Pin Assignments

| Define | GPIO | LED |
|--------|------|-----|
| `A_RX` | 27 | Room A Receive |
| `A_AWAKE` | 14 | Room A Awake |
| `B_RX` | 32 | Room B Receive |
| `B_AWAKE` | 33 | Room B Awake |
| `C_RX` | 4 | Room C Receive |
| `C_AWAKE` | 16 | Room C Awake |
| `HUB_ERROR` | 22 | Error |
| `HUB_RECEIVE` | 19 | Data Sent / TX to MATLAB |

### Node Pin Assignments (same on all 3 nodes)

| Function | GPIO |
|----------|------|
| BH1750 SDA | 26 |
| BH1750 SCL | 27 |
| DHT22 Data | 25 |

### Boot Sequence

When the gateway powers on, it runs through a little self test before doing anything else: every LED pin gets forced LOW first (some GPIO pins can come up HIGH for a moment on boot before any code runs, so this clears that out), then each LED lights up one at a time, then all 8 flash together twice, then everything turns off and normal operation starts.

### LED Resistors

| Colour | Forward Voltage |
|--------|------------------|
| Red | 2.0-2.2V |
| Blue | 3.0-3.2V |
| White | 3.0-3.2V |
| Yellow | 1.8-2.0V |
| Green | 3.0-3.2V |
| Orange | 1.8-2.0V |

All LEDs are rated for 20mA forward current. A single 470Ω resistor value was used for every LED regardless of colour, just for simplicity. This means they're not all running at the same brightness or current, but it's not really an issue since these are just status indicators and don't need to be especially bright.

---

## Message Protocol

All messages are sent line by line from the gateway, to both Serial and to MATLAB over TCP at the same time.

```
STATUS:GATEWAY_READY
STATUS:ROOMA_SLEEPING:duration=360
STATUS:ROOMA_RECONNECTED
ERROR:ROOMB_TIMEOUT
ERROR:ROOMC_DHT_FAILURE
ERROR:ROOMC_BH1750_FAILURE
DATA:ROOMA:24.50,68.90,73.33
```

- `DATA:<ROOM>:<temperature>,<humidity>,<lux>`, one line per node per reading
- `STATUS:<ROOM>_SLEEPING:duration=<seconds>`, sent right before a node powers down
- `STATUS:<ROOM>_RECONNECTED`, sent if a node that had been flagged unreachable sends data again
- `ERROR:<ROOM>_TIMEOUT`, a node missed its expected wake up window
- `ERROR:<ROOM>_DHT_FAILURE` / `ERROR:<ROOM>_BH1750_FAILURE`, sent alongside the data line when a sensor on an otherwise working node fails

---

## Timeout / Error Logic

A fixed timeout doesn't really work once nodes are sleeping for minutes at a time, it would keep falsely triggering during completely normal sleep. So instead:

- When a sleep message comes in, the gateway works out `expected_wake_time = millis() + (sleep_duration_sec * 1000) + GRACE_WINDOW_MS`
- `ERROR:ROOMX_TIMEOUT` only fires if that node hasn't sent a data packet by its own `expected_wake_time`
- Any data packet clears the unreachable flag and resets the expected wake time back to 0 until the next sleep message
- If a node comes back after being flagged unreachable, the gateway sends `STATUS:ROOMX_RECONNECTED`

**Current values:**
- Sleep interval: 360 seconds (6 minutes), 10 readings an hour
- Grace window: 25 seconds, picked to comfortably cover a typical wake up to send cycle (roughly 2-3 seconds: booting, sensor settling, reconnecting WiFi/ESP-NOW, reading, sending) with some buffer for slower cycles, while still being small compared to the 6 minute interval

---

## MATLAB Responsibilities

- Connects to the gateway with `tcpclient(GATEWAY_IP, 8080)`
- Reads incoming lines and checks the prefix:
  - `STATUS:` gets printed to console with a timestamp and written to the log file
  - `ERROR:` gets printed to console (in red, via stderr) and written to the log file
  - `DATA:` gets parsed into temperature/humidity/lux per room and the live plots get updated
- Logs every `STATUS`/`ERROR` message into `sensor_log.txt`, which keeps appending across sessions
- Plots live data in 3 stacked subplots (temperature, humidity, light), each one showing all 3 rooms as separate lines, with a rolling window of recent points

> The gateway's IP address can change depending on whatever network it joins, since it's assigned by DHCP. Check Serial Monitor each session and update the IP in the MATLAB script if needed.

---

## Debugging / Errors Encountered

A running list of the actual problems hit while building this, since most of them weren't obvious from the code alone.

**LED wouldn't light up no matter what**
Spent ages checking polarity, resistor values, GPIO pin mapping, even swapped pins entirely. Eventually traced it to the breadboard itself: long breadboards have a physical gap splitting the ground rail into two separate halves that aren't actually connected to each other. The ESP32's ground was in one half and the LED circuit's ground was in the other half, so they were never connected despite looking like the same rail. Fixed by bridging both halves with a jumper wire. Worth doing this on any new breadboard build before debugging anything else.

**BH1750 "device not configured" error, but only sometimes**
The BH1750 worked fine on a direct 6V supply but kept failing with this error once running off USB. Turned out both sensors had their VCC wired to VIN instead of 3.3V. VIN is unregulated so it just passes through whatever is feeding it, meaning the BH1750 (rated for 2.4 to 3.6V) was actually running at close to 5 or 6V depending on the power source. It happened to behave at 6V but not at 5V from USB, which made it look inconsistent rather than just wrong. Moving both sensors' VCC to the 3.3V pin fixed it completely. This also matters for the battery powered nodes specifically, since a sensor wired to VIN would get less reliable as the battery drains.

**GPIO14 turning on by itself before any code ran**
Powered on the gateway and noticed one LED was already lit before any sketch had even done anything. Some ESP32 GPIO pins default to a HIGH state for a moment during boot, before `setup()` runs. Fixed by forcing every LED pin LOW right at the very start of the boot sequence function.

**ESP-NOW callback signature error after an IDF update**
Got a compile error about the send callback signature not matching. Newer versions of the ESP32 Arduino core changed the callback from taking a MAC address to taking a `wifi_tx_info_t` struct instead. Just had to update the callback function signature to match.

**Error LED stuck blinking even while a node was asleep**
After adding deep sleep, the gateway kept showing a sensor fault for a node that was just sleeping normally. This was because the gateway didn't yet know how to tell apart a sleep announcement from a regular data packet, so the last reading it saw before the node went quiet (which happened to have a sensor fault from sensors not being settled yet) just stayed stuck. Fixed by adding sensor settling delays on the node side, and by having the gateway properly handle `MSG_SLEEP` messages separately from `MSG_DATA`.

---

## Future Features (Not Built Yet)

- **Remote reset for all nodes:** thought about having the gateway broadcast a reset command over ESP-NOW, but since nodes are asleep most of the time and ESP-NOW only reaches devices that are actually awake, this wouldn't reliably reset a sleeping node. Would need actual hardware (something like a relay or transistor on the gateway pulling each node's EN pin low) to force a reset regardless of sleep state. For now just resetting each node by hand during testing.
- **Rebuilding a day's data from the log file:** a MATLAB script that could take `sensor_log.txt` and reconstruct/replay what happened on any given day, instead of only being able to see things live.

---

## Confirmed Working

- ESP-NOW and the gateway's WiFi connection working together on channel 1, tested with both a phone hotspot and home WiFi
- All 3 nodes sending data and sleep announcements correctly
- Gateway correctly telling apart sensor faults from unreachable nodes, with the right LED priority
- Full pipeline working end to end: node, ESP-NOW, gateway, TCP, MATLAB, including live plotting and logging

---

## Build Order (roughly how it actually went)

1. Sensor node firmware: reading sensors, then ESP-NOW transmission
2. Gateway firmware: ESP-NOW receiving, LEDs (RX, then error/fault states, then awake/asleep), boot sequence
3. Deep sleep and sleep announcements on the nodes, expected wake window timeout logic on the gateway
4. Gateway WiFi connection and channel check
5. Gateway TCP server and the message protocol
6. MATLAB script: connecting, parsing, logging, live plotting
