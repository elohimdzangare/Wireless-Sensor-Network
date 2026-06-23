#include <WiFi.h>
#include <esp_wifi.h>

const char* ssid     = "FBI Wiretap #21";     // Mobile Hotspot name
const char* password = "pleasedontconnect";   // Mobile hotspot password

void setup() {
  Serial.begin(115200);
  delay(1000);

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

  // Find out which WiFi channel we landed on
  uint8_t primaryChan;
  wifi_second_chan_t secondChan;
  esp_wifi_get_channel(&primaryChan, &secondChan);
  Serial.print("Connected on WiFi channel: ");
  Serial.println(primaryChan);
}

void loop() {
  // nothing (just checking connection + channel)
}