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

// Data packet structure (matches node exactly)
typedef struct {
  uint8_t node_id;
  float   temperature;
  float   humidity;
  float   lux;
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

// ESP-NOW receive callback
void onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  SensorPacket packet;
  memcpy(&packet, data, sizeof(packet));

  Serial.printf("--- Packet from Node %d ---\n", packet.node_id);
  Serial.printf("Temperature: %.2f °C\n", packet.temperature);
  Serial.printf("Humidity:    %.2f %%\n", packet.humidity);
  Serial.printf("Light:       %.2f lux\n", packet.lux);
  Serial.println("--------------------------");

  // Flash the corresponding RX LED
  int rxPin;
  switch (packet.node_id) {
    case 1: rxPin = A_RX; break;
    case 2: rxPin = B_RX; break;
    case 3: rxPin = C_RX; break;
    default: return; // unknown node_id, ignore
  }

  digitalWrite(rxPin, HIGH);
  delay(100);
  digitalWrite(rxPin, LOW);
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
  // nothing (everything handled in callback)
}