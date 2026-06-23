#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>

// Pin definitions
#define SDA_PIN   26
#define SCL_PIN   27
#define DHT_PIN   25
#define DHT_TYPE  DHT22

// Sensor objects
BH1750 lightMeter;
DHT dht(DHT_PIN, DHT_TYPE);

void setup() {
  Serial.begin(115200);
  
  // Initialise I2C with custom pins
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialise BH1750
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 initialised successfully");
  } else {
    Serial.println("BH1750 init failed — check wiring");
  }
  
  // Initialise DHT22
  dht.begin();
  Serial.println("DHT22 initialised");
  Serial.println("----------------------------");
}

void loop() {
  // Read BH1750
  float lux = lightMeter.readLightLevel();
  
  // Read DHT22 (needs at least 2 seconds between readings)
  float temperature = dht.readTemperature();
  float humidity    = dht.readHumidity();
  
  // Check for DHT22 read failure
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT22 read failed — check wiring");
  } else {
    Serial.print("Temperature: "); Serial.print(temperature); Serial.println(" °C");
    Serial.print("Humidity:    "); Serial.print(humidity);    Serial.println(" %");
    Serial.print("Light:       "); Serial.print(lux);         Serial.println(" lux");
    Serial.println("----------------------------");
  }
  
  delay(2000); // DHT22 needs 2 seconds minimum between reads
}