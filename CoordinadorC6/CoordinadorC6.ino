#ifndef ZIGBEE_MODE_ZCZR
#error "Selecciona Herramientas -> Zigbee mode -> Zigbee ZCZR (coordinator/router)"
#endif

#include <Arduino.h>
#include "Zigbee.h"
#include "esp_system.h"

#define COORD_ENDPOINT 1

ZigbeeSwitch zbCoord(COORD_ENDPOINT);   // endpoint temporal, luego lo cambiamos

unsigned long lastPrint = 0;

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n=== ESP32-C6 ZIGBEE COORDINATOR ===");
  Serial.printf("Reset reason: %d\n", esp_reset_reason());

  zbCoord.setManufacturerAndModel("UAEH", "RFID_Coord");
  Zigbee.addEndpoint(&zbCoord);

  Zigbee.setRebootOpenNetwork(180);   // red abierta 180 s tras arrancar

  Serial.println("Starting Zigbee...");
  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
    Serial.println("ERROR: Zigbee.begin failed -> revisa Zigbee mode y Partition Scheme");
    delay(3000);
    ESP.restart();
  }
  Serial.println("Zigbee stack started");
}

void loop() {
  if (millis() - lastPrint >= 2000) {
    lastPrint = millis();
    Serial.printf("Uptime: %lu s | Started: %s | Connected: %s\n",
                  millis() / 1000,
                  Zigbee.started() ? "YES" : "NO",
                  Zigbee.connected() ? "YES" : "NO");
  }
  delay(100);
}
