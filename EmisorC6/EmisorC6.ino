// EmisorC6: lee etiquetas RFID UHF (YRM10002 por UART) y transmite su EPC por Zigbee
// al coordinador (CoordinadorC6). Ver PROTOCOLO_RFID_ZIGBEE.md.
//
// Arduino IDE: Board "ESP32C6 Dev Module", Zigbee mode "Zigbee ZCZR (coordinator/router)"
// (igual que el actuador del ejemplo que ya funciona), partición y flash como en el ejemplo.
//
// Concurrencia:
//   loop()        -> SOLO lee UART y parsea (nunca espera a Zigbee). Encola EPC detectados.
//   zigbeeTask    -> inicia Zigbee, espera conexión y envía lo que hay en la cola.
// Si Zigbee no está listo, loop() sigue leyendo; la cola descarta lo más antiguo.

#ifndef ZIGBEE_MODE_ZCZR
#ifndef ZIGBEE_MODE_ED
#error "Selecciona Tools -> Zigbee mode -> Zigbee ZCZR (igual que el actuador del ejemplo)"
#endif
#endif

#include <Arduino.h>
#include "esp_system.h"
#include "rfid_zigbee_ep.h"
#include "yrm_parser.h"

// ---------------- MODO DE PRUEBA (cambiar SOLO esta constante y recompilar) ----------------
// true : genera EPC ficticios cada SIM_INTERVAL_MS y los envía por Zigbee con el mismo mecanismo
//        que los EPC reales (misma cola, protocolo, comando 0xFC10, destino y callbacks).
//        No se envía el inventario al lector ni se procesa su UART. Los mensajes llevan
//        FLAG_SIMULATED y el coordinador imprime "ORIGEN: SIMULADO".
// false: lectura real con el YRM10002 (comportamiento original).
constexpr bool RFID_SIMULATION_MODE = true;
constexpr unsigned long SIM_INTERVAL_MS = 1000;

// ---------------- Lector RFID (sin cambios respecto al original) ----------------
HardwareSerial RFID(1);

#define RFID_RX 4
#define RFID_TX 5
#define RFID_BAUD 115200

const uint8_t INVENTORY_CMD[] = {
  0xBB, 0x00, 0x27, 0x00, 0x03,
  0x22, 0x27, 0x10, 0x83, 0x7E
};

yrm::Parser parser;

// Si el lector calla este tiempo (el inventario múltiple de 10000 rondas terminó, o se reinició),
// se reenvía el MISMO comando de inventario. No altera el protocolo con el lector.
const unsigned long READER_SILENCE_MS = 5000;
unsigned long lastReaderByte = 0;

// ---------------- Zigbee: configuración tomada del ejemplo funcional ----------------
const uint8_t CANAL_ASIGNADO = 11;  // debe coincidir con CoordinadorC6

// Rol efectivo del actuador del ejemplo que ya funciona: Zigbee.begin() sin argumentos =
// ZIGBEE_END_DEVICE (default del core 3.3.8), con rx_on_when_idle=true (no duerme).
// Se declara explícito para que el código diga lo que ya se probaba. Cambiar a ZIGBEE_ROUTER
// solo tras validar en hardware.
const zigbee_role_t ROL_ZIGBEE = ZIGBEE_END_DEVICE;

ZigbeeRfidEP zbRfid(rfidzb::SENDER_ENDPOINT, /*serverRole=*/false);

// ---------------- Cola lector -> Zigbee ----------------
struct TagItem {
  uint8_t epc[rfidzb::EPC_MAX];
  uint8_t epcLen;
  int8_t rssi;
  bool simulated;  // true = dato ficticio del modo de prueba
};
const size_t QUEUE_LEN = 8;
QueueHandle_t tagQueue = nullptr;
volatile uint32_t droppedQueue = 0;  // descartes por cola llena (se descarta el más antiguo)

// Deduplicación: el lector repite la misma etiqueta muchas veces por segundo.
const unsigned long DEDUP_MS = 1000;
uint8_t lastEpc[rfidzb::EPC_MAX];
uint8_t lastEpcLen = 0;
unsigned long lastEpcTime = 0;

// ---------------- Confirmación de entrega (callback en la tarea del stack Zigbee) ----------------
struct SendStatus {
  esp_err_t status;
  uint8_t tsn;
};
QueueHandle_t statusQueue = nullptr;

// Se ejecuta en el contexto del stack Zigbee: NO imprimir ni bloquear; solo encolar.
static void onSendStatus(esp_zb_zcl_command_send_status_message_t message) {
  if (statusQueue == nullptr) return;
  SendStatus s = {message.status, message.tsn};
  xQueueSend(statusQueue, &s, 0);
}

static void enqueueItem(const TagItem &it);

static void printHex(const uint8_t *b, size_t n, char *out, size_t cap) {
  rfidzb::epcToHex(b, n, out, cap);
}

// ---------------- Tarea Zigbee ----------------
static void zigbeeTask(void *) {
  Serial.println("[ZIGBEE] Inicializando dispositivo");

  if (!Zigbee.begin(ROL_ZIGBEE)) {
    Serial.println("[ZIGBEE] ERROR: Zigbee.begin() fallo (revisa Zigbee mode / particion). Reiniciando en 5 s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP.restart();
  }

  Serial.print("[ZIGBEE] Buscando coordinador en canal ");
  Serial.print(CANAL_ASIGNADO);
  unsigned long t0 = millis();
  while (!Zigbee.connected()) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    // Tras 3 minutos sin red, reiniciar: reintenta la unión desde cero (recuperación simple).
    if (millis() - t0 > 180000UL) {
      Serial.println("\n[ZIGBEE] Sin coordinador tras 180 s. Reiniciando");
      ESP.restart();
    }
  }
  Serial.println();
  Serial.println("[ZIGBEE] Conectado al coordinador");

  // Registrado con el stack ya corriendo; el core no usa este handler.
  esp_zb_zcl_command_send_status_handler_register(onSendStatus);

  uint16_t nextId = 1;
  uint32_t sentAccepted = 0, deliveryOk = 0, deliveryFail = 0;

  for (;;) {
    TagItem it;
    if (xQueueReceive(tagQueue, &it, pdMS_TO_TICKS(200)) == pdTRUE) {
      char hex[2 * rfidzb::EPC_MAX + 1];
      printHex(it.epc, it.epcLen, hex, sizeof(hex));

      if (!Zigbee.connected()) {
        Serial.printf("[ZIGBEE] Sin red: EPC %s descartado\n", hex);
      } else {
        rfidzb::TagMsg m;
        memset(&m, 0, sizeof(m));
        m.msgId = nextId;
        m.rssi = it.rssi;
        m.flags = rfidzb::FLAG_RSSI_VALID | (it.simulated ? rfidzb::FLAG_SIMULATED : 0);
        m.epcLen = it.epcLen;
        memcpy(m.epc, it.epc, it.epcLen);

        if (it.simulated) {
          // El msgId se asigna aquí (al enviar), por eso el bloque [SIM] se imprime en este punto.
          Serial.println();
          Serial.println("[SIM] Generando RFID ficticio");
          Serial.printf("[SIM] EPC: %s\n", hex);
          Serial.printf("[SIM] RSSI: %d\n", (int)it.rssi);
          Serial.printf("[SIM] MSG ID: %u\n", (unsigned)nextId);
          Serial.println("[ZIGBEE] Enviando mensaje...");
        } else {
          Serial.printf("[ZIGBEE] Enviando RFID (id %u)...\n", (unsigned)nextId);
        }
        if (zbRfid.sendTag(m)) {
          sentAccepted++;
          nextId++;  // wrap natural de uint16_t
          Serial.println("[ZIGBEE] Transmision aceptada por el stack (entrega pendiente de confirmacion)");
        } else {
          Serial.println("[ZIGBEE] Transmision RECHAZADA por el stack (lock ocupado o mensaje invalido)");
        }
      }
    }

    // Resultados asíncronos de entrega (APS ack del destino).
    SendStatus s;
    while (xQueueReceive(statusQueue, &s, 0) == pdTRUE) {
      if (s.status == ESP_OK) {
        deliveryOk++;
        Serial.printf("[ZIGBEE] Entrega confirmada por la red (tsn %u) | aceptadas:%lu ok:%lu fallo:%lu\n",
                      s.tsn, (unsigned long)sentAccepted, (unsigned long)deliveryOk, (unsigned long)deliveryFail);
      } else {
        deliveryFail++;
        Serial.printf("[ZIGBEE] Entrega FALLIDA (tsn %u, err 0x%x) | aceptadas:%lu ok:%lu fallo:%lu\n",
                      s.tsn, (unsigned)s.status, (unsigned long)sentAccepted, (unsigned long)deliveryOk, (unsigned long)deliveryFail);
      }
    }
  }
}

// ---------------- Lector: detección -> cola ----------------
static void onTagDetected(const yrm::Tag &tag) {
  const unsigned long now = millis();
  const bool same = (tag.epcLen == lastEpcLen) && (memcmp(tag.epc, lastEpc, tag.epcLen) == 0);
  if (same && (now - lastEpcTime) < DEDUP_MS) return;  // misma etiqueta, ya reportada hace poco

  lastEpcLen = tag.epcLen;
  memcpy(lastEpc, tag.epc, tag.epcLen);
  lastEpcTime = now;

  char hex[2 * rfidzb::EPC_MAX + 1];
  printHex(tag.epc, tag.epcLen, hex, sizeof(hex));
  Serial.printf("\n[RFID] EPC detectado: %s | RSSI: %d dBm\n", hex, (int)tag.rssi);

  TagItem it;
  memset(&it, 0, sizeof(it));
  it.epcLen = tag.epcLen;
  it.rssi = tag.rssi;
  it.simulated = false;
  memcpy(it.epc, tag.epc, tag.epcLen);
  enqueueItem(it);
}

// ---------------- Modo simulación: genera EPC ficticios y los mete en la MISMA cola ----------------
static void generateSimulatedTag() {
  static uint32_t n = 0;
  // EPC de 12 bytes = E28068940000501234567890 + n, con los 2 últimos bytes como contador (78 90, 78 91, ...)
  static const uint8_t BASE[12] = {0xE2, 0x80, 0x68, 0x94, 0x00, 0x00, 0x50, 0x12, 0x34, 0x56, 0x78, 0x90};
  TagItem it;
  memset(&it, 0, sizeof(it));
  memcpy(it.epc, BASE, sizeof(BASE));
  const uint16_t tail = (uint16_t)(((uint16_t)BASE[10] << 8 | BASE[11]) + n);
  it.epc[10] = (uint8_t)(tail >> 8);
  it.epc[11] = (uint8_t)(tail & 0xFF);
  it.epcLen = sizeof(BASE);
  it.rssi = (int8_t)(-45 - (int)((n * 2) % 16));  // -45..-59, variación determinista
  it.simulated = true;
  n++;
  enqueueItem(it);
}

// Nunca bloquea: si la cola está llena se descarta el elemento más antiguo.
static void enqueueItem(const TagItem &it) {
  if (xQueueSend(tagQueue, &it, 0) != pdTRUE) {
    TagItem old;
    xQueueReceive(tagQueue, &old, 0);
    xQueueSend(tagQueue, &it, 0);
    droppedQueue = droppedQueue + 1;  // un solo escritor (loop)
    Serial.printf("[RFID] Cola llena: descartado el mas antiguo (total descartes: %lu)\n", (unsigned long)droppedQueue);
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

  Serial.println();
  if (RFID_SIMULATION_MODE) {
    Serial.println("====================================");
    Serial.println("       RFID SIMULATION MODE");
    Serial.println("====================================");
  } else {
    Serial.println("=================================");
    Serial.println("        RFID ZIGBEE SENDER");
    Serial.println("=================================");
  }
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());

  tagQueue = xQueueCreate(QUEUE_LEN, sizeof(TagItem));
  statusQueue = xQueueCreate(8, sizeof(SendStatus));
  if (tagQueue == nullptr || statusQueue == nullptr) {
    Serial.println("[SISTEMA] ERROR: sin memoria para colas. Reiniciando");
    delay(3000);
    ESP.restart();
  }

  lastReaderByte = millis();
  if (RFID_SIMULATION_MODE) {
    Serial.printf("[SIM] Lector RFID desactivado; EPC ficticios cada %lu ms\n", SIM_INTERVAL_MS);
  } else {
    RFID.write(INVENTORY_CMD, sizeof(INVENTORY_CMD));
    Serial.println("[RFID] Lector inicializado");
  }

  // Registro Zigbee (rápido, no bloquea). El arranque de la red va en zigbeeTask.
  zbRfid.setManufacturerAndModel("UAEH", "RFID_Sender");
  Zigbee.addEndpoint(&zbRfid);
  Zigbee.setPrimaryChannelMask(1 << CANAL_ASIGNADO);

  xTaskCreate(zigbeeTask, "zb_sender", 6144, nullptr, 5, nullptr);
}

void loop() {
  if (RFID_SIMULATION_MODE) {
    // Sin delay(): temporización por millis(). Solo se genera con red, para no acumular datos viejos.
    static unsigned long lastSim = 0;
    if (Zigbee.connected() && millis() - lastSim >= SIM_INTERVAL_MS) {
      lastSim = millis();
      generateSimulatedTag();
    }
    delay(1);
    return;
  }

  while (RFID.available()) {
    uint8_t b = RFID.read();
    lastReaderByte = millis();

    switch (parser.feed(b)) {
      case yrm::Event::TAG:
        onTagDetected(parser.tag());
        break;
      case yrm::Event::BAD_CHECKSUM:
        Serial.println("Checksum incorrecto");  // mismo mensaje que el original
        break;
      case yrm::Event::READER_ERROR:
        // 0x15 = sin etiquetas en el campo: es normal durante el inventario, no se imprime.
        if (parser.errorCode() != 0x15) {
          Serial.printf("Respuesta de error del lector (codigo 0x%02X)\n", parser.errorCode());
        }
        break;
      default:
        break;
    }
  }

  // El inventario múltiple termina tras 10000 rondas: si el lector calla, se reanuda.
  if (millis() - lastReaderByte > READER_SILENCE_MS) {
    Serial.println("[RFID] Lector en silencio: reenviando comando de inventario");
    RFID.write(INVENTORY_CMD, sizeof(INVENTORY_CMD));
    lastReaderByte = millis();
  }

  delay(1);  // cede CPU; a 115200 baud el buffer UART (256 B por defecto) aguanta de sobra
}
