#include <WiFi.h>
#include <esp_now.h>

// Pin definitions
#define A_RX       27
#define A_AWAKE    14
#define B_RX       32
#define B_AWAKE    33
#define C_RX        4
#define C_AWAKE    16
#define HUB_ERROR  22
#define HUB_RECEIVE 19

// LED flash duration (non-blocking)
const unsigned long LED_FLASH_DURATION = 100;

// Grace window added on top of announced sleep duration before flagging unreachable
const unsigned long GRACE_WINDOW_MS = 25000; // 25 sec

// Message types (must match node sketch)
#define MSG_DATA  0
#define MSG_SLEEP 1

// Track RX LED on-times per node (index 0=A, 1=B, 2=C)
unsigned long rxLedOnTime[3] = {0, 0, 0};
int rxPins[3] = {A_RX, B_RX, C_RX};

// Awake/Asleep LED pins per node
int awakePins[3] = {A_AWAKE, B_AWAKE, C_AWAKE};

// Per-node sensor fault tracking
bool sensorFault[3] = {false, false, false};

// Per-node unreachable tracking
bool nodeUnreachable[3] = {false, false, false};

// Per-node expected wake time (0 = not currently sleeping / unknown)
unsigned long expectedWakeTime[3] = {0, 0, 0};

// Error LED state machine
enum ErrorState { ERROR_NONE, ERROR_SENSOR_FAULT, ERROR_NODE_UNREACHABLE };
ErrorState currentErrorState = ERROR_NONE;

unsigned long lastBlinkToggle = 0;
bool blinkState = false;
const unsigned long BLINK_INTERVAL = 500; // slow blink, 500ms on/off

// Data packet structure (matches node exactly)
typedef struct {
  uint8_t  node_id;
  uint8_t  msg_type;
  float    temperature;
  float    humidity;
  float    lux;
  bool     dht_ok;
  bool     bh1750_ok;
  uint32_t sleep_duration_sec;
} SensorPacket;

void bootSequence() {
  int pins[] = {A_RX, A_AWAKE, B_RX, B_AWAKE, C_RX, C_AWAKE, HUB_ERROR, HUB_RECEIVE};
  int numPins = 8;

  // All pins set as OUTPUT and force LOW first
  for (int i = 0; i < numPins; i++) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);
  }
  delay(300);

  // Light up each LED one by one
  for (int i = 0; i < numPins; i++) {
    digitalWrite(pins[i], HIGH);
    delay(200);
  }

  // Flash all twice
  for (int flash = 0; flash < 2; flash++) {
    for (int i = 0; i < numPins; i++) digitalWrite(pins[i], LOW);
    delay(300);
    for (int i = 0; i < numPins; i++) digitalWrite(pins[i], HIGH);
    delay(300);
  }

  // All off (normal operation begins)
  for (int i = 0; i < numPins; i++) digitalWrite(pins[i], LOW);
  delay(300);

  Serial.println("Boot sequence complete");
}

// Re-evaluates overall error state based on per-node flags
void evaluateErrorState() {
  bool anyUnreachable = false;
  bool anySensorFault = false;

  for (int i = 0; i < 3; i++) {
    if (nodeUnreachable[i]) anyUnreachable = true;
    if (sensorFault[i])     anySensorFault = true;
  }

  if (anyUnreachable) {
    currentErrorState = ERROR_NODE_UNREACHABLE;
  } else if (anySensorFault) {
    currentErrorState = ERROR_SENSOR_FAULT;
  } else {
    currentErrorState = ERROR_NONE;
  }
}

// Drives the physical Error LED based on currentErrorState (called every loop())
void updateErrorLED() {
  switch (currentErrorState) {
    case ERROR_NONE:
      digitalWrite(HUB_ERROR, LOW);
      break;

    case ERROR_SENSOR_FAULT:
      if (millis() - lastBlinkToggle >= BLINK_INTERVAL) {
        blinkState = !blinkState;
        digitalWrite(HUB_ERROR, blinkState);
        lastBlinkToggle = millis();
      }
      break;

    case ERROR_NODE_UNREACHABLE:
      digitalWrite(HUB_ERROR, HIGH);
      break;
  }
}

// Checks each node's expected wake time and flags it unreachable if missed
void checkNodeTimeouts() {
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (expectedWakeTime[i] != 0 && now > expectedWakeTime[i] && !nodeUnreachable[i]) {
      nodeUnreachable[i] = true;
      Serial.printf("ERROR: Node %d missed its expected wake window\n", i + 1);
      evaluateErrorState();
    }
  }
}

// ESP-NOW receive callback
void onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  SensorPacket packet;
  memcpy(&packet, data, sizeof(packet));

  int idx = packet.node_id - 1;
  if (idx < 0 || idx > 2) return; // unknown node_id, ignore

  if (packet.msg_type == MSG_SLEEP) {
    // Node announcing it's about to sleep
    unsigned long sleepMs = (unsigned long)packet.sleep_duration_sec * 1000UL;
    expectedWakeTime[idx] = millis() + sleepMs + GRACE_WINDOW_MS;

    digitalWrite(awakePins[idx], LOW); // Awake LED off (node is going to sleep)
    Serial.printf("STATUS: Node %d sleeping for %lu sec\n", packet.node_id, packet.sleep_duration_sec);
    return;
  }

  // msg_type == MSG_DATA
  Serial.printf("--- Packet from Node %d ---\n", packet.node_id);

  if (packet.dht_ok) {
    Serial.printf("Temperature: %.2f °C\n", packet.temperature);
    Serial.printf("Humidity:    %.2f %%\n", packet.humidity);
  } else {
    Serial.println("DHT22: SENSOR FAULT");
  }

  if (packet.bh1750_ok) {
    Serial.printf("Light:       %.2f lux\n", packet.lux);
  } else {
    Serial.println("BH1750: SENSOR FAULT");
  }
  Serial.println("--------------------------");

  // Node just sent data — it's awake and reachable
  digitalWrite(awakePins[idx], HIGH);
  digitalWrite(rxPins[idx], HIGH);
  rxLedOnTime[idx] = millis();

  // Clear unreachable state — node is clearly alive
  if (nodeUnreachable[idx]) {
    Serial.printf("STATUS: Node %d reconnected\n", packet.node_id);
  }
  nodeUnreachable[idx] = false;
  expectedWakeTime[idx] = 0; // no longer "asleep on schedule" until next sleep announcement

  // Update sensor fault flag for this node
  sensorFault[idx] = (!packet.dht_ok || !packet.bh1750_ok);

  evaluateErrorState();
}

void setup() {
  Serial.begin(115200);

  bootSequence();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);
  Serial.println("Gateway ready, waiting for packets...");
}

void loop() {
  // Non-blocking RX LED turn-off check
  for (int i = 0; i < 3; i++) {
    if (rxLedOnTime[i] != 0 && millis() - rxLedOnTime[i] >= LED_FLASH_DURATION) {
      digitalWrite(rxPins[i], LOW);
      rxLedOnTime[i] = 0;
    }
  }

  // Drive the Error LED according to current state
  updateErrorLED();

  // Check whether any sleeping node has missed its expected wake window
  checkNodeTimeouts();
}