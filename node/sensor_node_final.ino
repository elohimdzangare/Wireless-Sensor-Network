#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>

// Pin definitions
#define SDA_PIN  26
#define SCL_PIN  27
#define DHT_PIN  25
#define DHT_TYPE DHT22

// Node identity — change per node (1 = Room A, 2 = Room B, 3 = Room C)
#define NODE_ID 1

// Sleep interval for this node (seconds) — 360s = 6 min = 10 readings/hour
#define SLEEP_DURATION_SEC 360

// Message types
#define MSG_DATA  0
#define MSG_SLEEP 1

// Gateway MAC address
uint8_t gatewayMAC[] = {0x8C, 0x94, 0xDF, 0xB9, 0x9B, 0xC4};

// Data packet structure
typedef struct {
  uint8_t  node_id;
  uint8_t  msg_type;            // 0 = DATA, 1 = SLEEP_ANNOUNCEMENT
  float    temperature;         // -999 if DHT failed
  float    humidity;            // -999 if DHT failed
  float    lux;                 // -999 if BH1750 failed
  bool     dht_ok;
  bool     bh1750_ok;
  uint32_t sleep_duration_sec;  // only meaningful when msg_type == MSG_SLEEP
} SensorPacket;

// Sensor objects
BH1750 lightMeter;
DHT dht(DHT_PIN, DHT_TYPE);

// Track whether ESP-NOW send succeeded, so you know whether to sleep or not
volatile bool lastSendSuccess = false;
volatile bool sendComplete = false;

// ESP-NOW send callback
void onSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  lastSendSuccess = (status == ESP_NOW_SEND_SUCCESS);
  sendComplete = true;
  Serial.println(lastSendSuccess ? "Packet delivered successfully" : "Packet delivery failed");
}

// Blocks briefly until the ESP-NOW send callback fires, or times out
void waitForSendComplete(unsigned long timeoutMs = 1000) {
  unsigned long start = millis();
  while (!sendComplete && (millis() - start < timeoutMs)) {
    delay(5);
  }
}

void goToSleep() {
  // Announce sleep to the gateway before powering down
  SensorPacket sleepMsg = {};
  sleepMsg.node_id            = NODE_ID;
  sleepMsg.msg_type           = MSG_SLEEP;
  sleepMsg.sleep_duration_sec = SLEEP_DURATION_SEC;

  sendComplete = false;
  esp_now_send(gatewayMAC, (uint8_t *)&sleepMsg, sizeof(sleepMsg));
  waitForSendComplete();

  Serial.printf("Sleeping for %d seconds...\n", SLEEP_DURATION_SEC);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_SEC * 1000000ULL);
  esp_deep_sleep_start();
  // Execution never returns past this point (chip reboots on wake)
}

void setup() {
  Serial.begin(115200);
  delay(3000); // let power rails stabilise after wake

  // Initialise sensors
  Wire.begin(SDA_PIN, SCL_PIN);
  bool bh1750Init = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.println(bh1750Init ? "BH1750 initialised" : "BH1750 init failed");

  dht.begin();
  Serial.println("DHT22 initialised");

  // WiFi/ESP-NOW init
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    goToSleep(); // to prevent getting stuck, retry next cycle
    return;
  }

  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, gatewayMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    goToSleep();
    return;
  }
  Serial.println("Gateway peer registered");
  Serial.println("----------------------------");

  // Give sensors time to power up and stabilise after cold boot
  delay(2000);

  // Read sensors
  float lux         = lightMeter.readLightLevel();
  float temperature = dht.readTemperature();
  float humidity    = dht.readHumidity();

  // DHT22 might fail its first read after power-up thus, retry once
  if (isnan(temperature) || isnan(humidity)) {
    delay(500);
    temperature = dht.readTemperature();
    humidity    = dht.readHumidity();
  }

  bool dhtOk    = !(isnan(temperature) || isnan(humidity));
  bool bh1750Ok = (lux >= 0);

  // Build data packet
  SensorPacket packet = {};
  packet.node_id     = NODE_ID;
  packet.msg_type    = MSG_DATA;
  packet.temperature = dhtOk ? temperature : -999;
  packet.humidity    = dhtOk ? humidity    : -999;
  packet.lux         = bh1750Ok ? lux       : -999;
  packet.dht_ok      = dhtOk;
  packet.bh1750_ok   = bh1750Ok;

  if (dhtOk) {
    Serial.printf("Temp: %.2f°C  Humidity: %.2f%%  ", temperature, humidity);
  } else {
    Serial.print("DHT22 FAILED  ");
  }
  if (bh1750Ok) {
    Serial.printf("Lux: %.2f\n", lux);
  } else {
    Serial.println("BH1750 FAILED");
  }

  // Send data packet
  sendComplete = false;
  esp_now_send(gatewayMAC, (uint8_t *)&packet, sizeof(packet));
  waitForSendComplete();

  // Announce sleep and power down
  goToSleep();
}

void loop() {
  // Never reached — setup() handles everything, then sleeps
}