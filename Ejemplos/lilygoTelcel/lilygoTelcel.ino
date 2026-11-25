/**************************************************************
 * Potenciómetro -> MQTT por red celular
 * Placa: LilyGO T-SIM7070G
 **************************************************************/
#define TINY_GSM_MODEM_SIM7070

#define SerialMon Serial
#define SerialAT  Serial1

#include <TinyGsmClient.h>
#include <PubSubClient.h>

// ---------- Red y MQTT ----------
const char apn[]      = "internet.itelcel.com";
const char gprsUser[] = "itelcel";
const char gprsPass[] = "itelcel";

const char* broker = "broker.hivemq.com";
String topicPot;   // se arma en setup() con el IMEI para que sea único

// ---------- Hardware ----------
#define PWR_PIN  4
#define PIN_POT  34

// ---------- Parámetros del potenciómetro ----------
#define POT_UMBRAL           40      // cambio mínimo (0-4095) para publicar
#define POT_MIN_ENTRE_ENVIOS 500     // ms entre mensajes mientras giras
#define POT_LATIDO           60000L  // ms, publica aunque no cambie

// ---------- Objetos y variables ----------
TinyGsm       modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient  mqtt(client);

String   imei = "";
String   tipoDeRedActual;
uint32_t lastReconnectAttempt = 0;

int      potUltimoEnviado = -1000;
uint32_t potUltimaLectura = 0;
uint32_t potUltimoEnvio   = 0;

// ============================================================
// Tecnología de red (2G / LTE-M / NB-IoT)
// ============================================================
void identificarTecnologiaRed() {
  modem.sendAT("+CPSI?");
  if (modem.waitResponse(2000L, "+CPSI: ") == 1) {
    String r = modem.stream.readStringUntil('\n');
    r.trim();
    modem.waitResponse();

    if (r.startsWith("GSM"))              tipoDeRedActual = "2G (GPRS)";
    else if (r.startsWith("LTE CAT-M1"))  tipoDeRedActual = "4G (LTE-M)";
    else if (r.startsWith("LTE NB-IOT"))  tipoDeRedActual = "4G (NB-IoT)";
    else {
      int c = r.indexOf(',');
      tipoDeRedActual = (c > 0) ? r.substring(0, c) : r;
    }
    SerialMon.println("Red: " + tipoDeRedActual);
  } else {
    modem.waitResponse();
    SerialMon.println("No se pudo leer la tecnología de red.");
  }
}

// ============================================================
// MQTT
// ============================================================
boolean mqttConnect() {
  SerialMon.print("Conectando a MQTT...");
  if (!mqtt.connect(imei.c_str())) {
    SerialMon.println(" falló");
    return false;
  }
  SerialMon.println(" ¡éxito!");
  return true;
}

// ============================================================
// Potenciómetro
// ============================================================
int leerPotPromedio() {
  long suma = 0;
  for (int i = 0; i < 16; i++) {
    suma += analogRead(PIN_POT);
    delayMicroseconds(200);
  }
  return suma / 16;
}

void revisarPotenciometro() {
  uint32_t ahora = millis();
  if (ahora - potUltimaLectura < 100) return;   // lee cada 100 ms
  potUltimaLectura = ahora;

  int raw = leerPotPromedio();

  bool cambio      = abs(raw - potUltimoEnviado) >= POT_UMBRAL;
  bool puedeEnviar = (ahora - potUltimoEnvio) >= POT_MIN_ENTRE_ENVIOS;
  bool latido      = (ahora - potUltimoEnvio) >= POT_LATIDO;

  if ((cambio && puedeEnviar) || latido) {
    int   porcentaje = map(raw, 0, 4095, 0, 100);
    float voltaje    = (raw / 4095.0) * 3.3;

    String json = "{";
    json += "\"dispositivo\":\"" + imei + "\",";
    json += "\"pot_raw\":" + String(raw) + ",";
    json += "\"pot_porcentaje\":" + String(porcentaje) + ",";
    json += "\"voltaje\":" + String(voltaje, 2) + ",";
    json += "\"red\":\"" + tipoDeRedActual + "\"";
    json += "}";

    if (mqtt.publish(topicPot.c_str(), json.c_str(), true)) {  // retenido
      potUltimoEnviado = raw;
      potUltimoEnvio   = ahora;
      SerialMon.println("Pot -> " + json);
    }
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
  SerialMon.begin(115200);
  delay(10);

  pinMode(PIN_POT, INPUT);
  analogSetPinAttenuation(PIN_POT, ADC_11db);   // rango ~0-3.3 V

  // Encender el módem
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);
  delay(1000);
  digitalWrite(PWR_PIN, LOW);

  SerialMon.println("Esperando a que el módem inicie...");
  SerialAT.begin(115200, SERIAL_8N1, 26, 27);
  delay(6000);

  modem.restart();
  imei = modem.getIMEI();
  SerialMon.println("IMEI: " + imei);

  topicPot = "GsmClientTest/" + imei + "/potenciometro";
  SerialMon.println("Topic: " + topicPot);

  modem.setNetworkMode(2);    // Automático
  modem.setPreferredMode(3);  // LTE-M > NB-IoT > GSM

  mqtt.setServer(broker, 1883);
  // La red, los datos y MQTT se conectan/reconectan en loop()

  tipoDeRedActual = modem.getOperator();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  // 1) Red celular
  if (!modem.isNetworkConnected()) {
    SerialMon.println("Buscando red Telcel...");
    if (!modem.waitForNetwork(180000L, true)) return;
    identificarTecnologiaRed();
  }

  // 2) Datos (IP)
  if (!modem.isGprsConnected()) {
    SerialMon.print("Conectando al APN...");
    if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
      SerialMon.println(" falló");
      delay(10000);
      return;
    }
    SerialMon.print(" ¡IP asignada! : ");
    SerialMon.println(modem.localIP());
  }

  // 3) MQTT
  if (!mqtt.connected()) {
    uint32_t t = millis();
    if (t - lastReconnectAttempt > 10000L) {
      lastReconnectAttempt = t;
      if (mqttConnect()) lastReconnectAttempt = 0;
    }
    delay(100);
    return;
  }

  mqtt.loop();
  revisarPotenciometro();
}