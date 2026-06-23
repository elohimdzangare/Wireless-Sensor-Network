#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// WiFi hotspot credentials
const char* ssid     = "WiFi name";
const char* password = "WiFi password";

// TCP server
const uint16_t TCP_PORT = 8080;
WiFiServer server(TCP_PORT);
WiFiClient client;

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

// TX LED tracking (non-blocking, same pattern as RX LEDs)
unsigned long txLedOnTime = 0;

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

// Room name lookup for human-readable messages
const char* roomName(uint8_t node_id) {
  switch (node_id) {
    case 1: return "ROOMA";
    case 2: return "ROOMB";
    case 3: return "ROOMC";
    default: return "UNKNOWN";
  }
}

// Sends a line to Serial AND to the TCP client (if connected), with newline
void sendMessage(const String &msg) {
  Serial.println(msg);
  if (client && client.connected()) {
    client.println(msg);
    txLedOnTime = millis();
    digitalWrite(HUB_RECEIVE, HIGH);
  }
}

void bootSequence() {
  int pins[] = {A_RX, A_AWAKE, B_RX, B_AWAKE, C_RX, C_AWAKE, HUB_ERROR, HUB_RECEIVE};
  int numPins = 8;

  for (int i = 0; i < numPins; i++) {
    pinMode(pins[i], OUTPUT);
    digitalWrite(pins[i], LOW);
  }
  delay(300);

  for (int i = 0; i < numPins; i++) {
    digitalWrite(pins[i], HIGH);
    delay(200);
  }

  for (int flash = 0; flash < 2; flash++) {
    for (int i = 0; i < numPins; i++) digitalWrite(pins[i], LOW);
    delay(300);
    for (int i = 0; i < numPins; i++) digitalWrite(pins[i], HIGH);
    delay(300);
  }

  for (int i = 0; i < numPins; i++) digitalWrite(pins[i], LOW);
  delay(300);

  Serial.println("Boot sequence complete");
}

// Connects to the WiFi hotspot and prints IP + channel info
void connectToHotspot() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to hotspot");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  uint8_t primaryChan;
  wifi_second_chan_t secondChan;
  esp_wifi_get_channel(&primaryChan, &secondChan);
  Serial.print("Connected on WiFi channel: ");
  Serial.println(primaryChan);
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

// Drives the physical Error LED based on currentErrorState — call every loop()
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
      sendMessage("ERROR:" + String(roomName(i + 1)) + "_TIMEOUT");
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

  const char* room = roomName(packet.node_id);

  if (packet.msg_type == MSG_SLEEP) {
    unsigned long sleepMs = (unsigned long)packet.sleep_duration_sec * 1000UL;
    expectedWakeTime[idx] = millis() + sleepMs + GRACE_WINDOW_MS;

    digitalWrite(awakePins[idx], LOW);
    sendMessage("STATUS:" + String(room) + "_SLEEPING:duration=" + String(packet.sleep_duration_sec));
    return;
  }

  // msg_type == MSG_DATA
  if (!packet.dht_ok) {
    sendMessage("ERROR:" + String(room) + "_DHT_FAILURE");
  }
  if (!packet.bh1750_ok) {
    sendMessage("ERROR:" + String(room) + "_BH1750_FAILURE");
  }

  // Build the DATA line for this node
  String dataLine = "DATA:" + String(room) + ":" +
                     String(packet.temperature, 2) + "," +
                     String(packet.humidity, 2) + "," +
                     String(packet.lux, 2);
  sendMessage(dataLine);

  // Node just sent data — it's awake and reachable
  digitalWrite(awakePins[idx], HIGH);
  digitalWrite(rxPins[idx], HIGH);
  rxLedOnTime[idx] = millis();

  if (nodeUnreachable[idx]) {
    sendMessage("STATUS:" + String(room) + "_RECONNECTED");
  }
  nodeUnreachable[idx] = false;
  expectedWakeTime[idx] = 0;

  sensorFault[idx] = (!packet.dht_ok || !packet.bh1750_ok);

  evaluateErrorState();
}

void setup() {
  Serial.begin(115200);

  bootSequence();

  connectToHotspot();

  server.begin();
  Serial.println("TCP server started on port " + String(TCP_PORT));

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);

  sendMessage("STATUS:GATEWAY_READY");
}

void loop() {
  // Accept a new MATLAB client if none currently connected
  if (!client || !client.connected()) {
    WiFiClient newClient = server.available();
    if (newClient) {
      client = newClient;
      Serial.println("MATLAB client connected");
    }
  }

  // Non-blocking RX LED turn-off check
  for (int i = 0; i < 3; i++) {
    if (rxLedOnTime[i] != 0 && millis() - rxLedOnTime[i] >= LED_FLASH_DURATION) {
      digitalWrite(rxPins[i], LOW);
      rxLedOnTime[i] = 0;
    }
  }

  // Non-blocking TX LED turn-off check
  if (txLedOnTime != 0 && millis() - txLedOnTime >= LED_FLASH_DURATION) {
    digitalWrite(HUB_RECEIVE, LOW);
    txLedOnTime = 0;
  }

  // Drive the Error LED according to current state
  updateErrorLED();

  // Check whether any sleeping node has missed its expected wake window
  checkNodeTimeouts();
}