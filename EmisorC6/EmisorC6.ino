
#include <Arduino.h>

HardwareSerial RFID(1);

#define RFID_RX 4
#define RFID_TX 5
#define RFID_BAUD 115200

const uint8_t INVENTORY_CMD[] = {
  0xBB, 0x00, 0x27, 0x00, 0x03,
  0x22, 0x27, 0x10, 0x83, 0x7E
};

uint8_t frame[128];
int pos = 0;
int expectedLength = 0;

void processFrame() {

  if (frame[0] != 0xBB) return;

  uint8_t type = frame[1];
  uint8_t cmd  = frame[2];

  // Validar checksum
  uint8_t checksum = 0;

  for (int i = 1; i < expectedLength - 2; i++) {
    checksum += frame[i];
  }

  if (checksum != frame[expectedLength - 2]) {
    Serial.println("Checksum incorrecto");
    return;
  }

  // Respuesta de inventario
  if (type == 0x02 && cmd == 0x22) {

    uint8_t rssiRaw = frame[5];

    int8_t rssi = (int8_t)rssiRaw;

    uint16_t pc =
      ((uint16_t)frame[6] << 8) | frame[7];

    // PC indica la longitud del EPC en palabras de 16 bits
    int epcLength = ((pc >> 11) & 0x1F) * 2;

    Serial.print("EPC: ");

    for (int i = 0; i < epcLength; i++) {
      uint8_t b = frame[8 + i];

      if (b < 0x10) Serial.print("0");
      Serial.print(b, HEX);
    }

    Serial.printf(" | RSSI: %d dBm\n", rssi);
  }

  // Respuesta de error
  else if (type == 0x01 && cmd == 0xFF) {
    Serial.println("Respuesta de error del lector");
  }
}

void setup() {

  Serial.begin(115200);

  RFID.begin(
    RFID_BAUD,
    SERIAL_8N1,
    RFID_RX,
    RFID_TX
  );

  delay(1000);

  Serial.println("\nESP32-C6 + YRM10002");
  Serial.println("Iniciando inventario...");

  RFID.write(INVENTORY_CMD, sizeof(INVENTORY_CMD));
}

void loop() {

  while (RFID.available()) {

    uint8_t b = RFID.read();

    // Buscar inicio de trama
    if (pos == 0) {
      if (b != 0xBB) continue;
    }

    frame[pos++] = b;

    // Al recibir los primeros 5 bytes conocemos la longitud
    if (pos == 5) {

      uint16_t dataLength =
        ((uint16_t)frame[3] << 8) | frame[4];

      expectedLength = 7 + dataLength;

      if (expectedLength > sizeof(frame)) {
        pos = 0;
        expectedLength = 0;
      }
    }

    // Trama completa
    if (expectedLength > 0 && pos == expectedLength) {

      if (frame[pos - 1] == 0x7E) {
        processFrame();
      }

      pos = 0;
      expectedLength = 0;
    }

    // Protección del buffer
    if (pos >= sizeof(frame)) {
      pos = 0;
      expectedLength = 0;
    }
  }
}
