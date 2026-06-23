#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>
#include <WiFi.h>
#include <esp_now.h>

// Pin definitions
#define SDA_PIN  26
#define SCL_PIN  27
#define DHT_PIN  25
#define DHT_TYPE DHT22

// Gateway MAC address
uint8_t gatewayMAC[] = {0x8C, 0x94, 0xDF, 0xB9, 0x9B, 0xC4};

// Data packet structure
typedef struct {
  uint8_t node_id;
  float   temperature;
  float   humidity;
  float   lux;
} SensorPacket;

// Sensor objects
BH1750 lightMeter;
DHT dht(DHT_PIN, DHT_TYPE);

// ESP-NOW send callback: confirms if packet was delivered
void onSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Packet delivered successfully");
  } else {
    Serial.println("Packet delivery failed");
  }
}

void setup() {
  Serial.begin(115200);

  // Initialise sensors
  Wire.begin(SDA_PIN, SCL_PIN);
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 initialised");
  } else {
    Serial.println("BH1750 init failed");
  }
  dht.begin();
  Serial.println("DHT22 initialised");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Initialise ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  Serial.println("ESP-NOW initialised");

  // Register send callback
  esp_now_register_send_cb(onSent);

  // Register gateway as peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, gatewayMAC, 6);
  peerInfo.channel = 0;  // 0 = current channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
  Serial.println("Gateway peer registered");
  Serial.println("----------------------------");
}

void loop() {
  // Read sensors
  float lux         = lightMeter.readLightLevel();
  float temperature = dht.readTemperature();
  float humidity    = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT22 read failed");
    delay(2000);
    return;
  }

  // Build packet
  SensorPacket packet;
  packet.node_id     = 3;  // ID changed based on Room
  packet.temperature = temperature;
  packet.humidity    = humidity;
  packet.lux         = lux;

  // Print what will be sent
  Serial.printf("Sending — Temp: %.2f°C  Humidity: %.2f%%  Lux: %.2f\n",
                temperature, humidity, lux);

  // Send via ESP-NOW
  esp_now_send(gatewayMAC, (uint8_t *)&packet, sizeof(packet));

  delay(2000);
}