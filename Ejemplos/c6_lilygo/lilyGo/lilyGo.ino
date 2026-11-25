// LilyGO T-SIM7070G  ->  recibe por UART y lo muestra en el monitor serial
#define RX_PIN 32
#define TX_PIN 33

void setup() {
  Serial.begin(115200);                                  // monitor serial (USB)
  Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);     // UART desde el C6
  Serial.println("LilyGO lista, esperando datos...");
}

void loop() {
  if (Serial2.available()) {
    String msg = Serial2.readStringUntil('\n');
    Serial.print("Recibido: ");
    Serial.println(msg);
  }
}