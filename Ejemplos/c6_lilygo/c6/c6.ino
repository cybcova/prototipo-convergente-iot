// ESP32-C6-DevKitC-1  ->  envía "hola" cada segundo
#define TX_PIN 10
#define RX_PIN 11

void setup() {
  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
}

void loop() {
  Serial1.println("hola");
  delay(1000);
}