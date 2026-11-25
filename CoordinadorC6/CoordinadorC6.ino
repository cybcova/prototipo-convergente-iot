// CoordinadorC6: coordinador Zigbee que recibe los EPC enviados por EmisorC6 y los muestra
// por consola. Ver PROTOCOLO_RFID_ZIGBEE.md.
//
// Arduino IDE: Board "ESP32C6 Dev Module", Zigbee mode "Zigbee ZCZR (coordinator/router)",
// partición y flash igual que el coordinador del ejemplo que ya funciona.
//
// Base: Ejemplos/Zigbee_Rele-main/Zigbee_Coordinador (mismo canal, EP1, red abierta 180 s,
// begin(ZIGBEE_COORDINATOR)). Recepción por comando de cluster personalizado (sin binding).
// El callback corre en la tarea del stack Zigbee: solo copia datos a una cola (espera 0);
// toda impresión ocurre en loop().
//
// SALIDA (DIAG): ver el bloque "Canal de salida" más abajo. Por defecto se imprime con
// log_printf(), que usa el MISMO camino que los logs [V]/[I]/[D] del stack Zigbee.

#ifndef ZIGBEE_MODE_ZCZR
#error "Selecciona Tools -> Zigbee mode -> Zigbee ZCZR (coordinator/router)"
#endif

#include <Arduino.h>
#include "esp_system.h"
#include "rfid_zigbee_ep.h"
#include "trace_link.h"

// ---------------- Enlace serial hacia el LilyGO (mecanismo de Ejemplos/c6_lilygo/c6) ----------------
// UART1 del C6: 115200 8N1, TX=GPIO10 -> RX LilyGO (GPIO32), RX=GPIO11 <- TX LilyGO (GPIO33, solo ACK).
// Un mensaje por línea ASCII terminada en "\r\n" (println), ahora con '$', checksum y secuencia.
// No bloquea: el callback Zigbee no toca la UART; loop() solo escribe si hay espacio en el TX buffer.
#define LILYGO_TX_PIN 10
#define LILYGO_RX_PIN 11
#define LILYGO_BAUD 115200

// ---------------- Canal de salida ----------------
// Causa del problema original (core esp32 3.3.8, ESP32-C6): Serial (con "USB CDC On Boot: Disabled")
// es UART0 por controlador, mientras que los logs del core/stack salen por ets_printf, cuya
// consola es UART0 y, además, la consola SECUNDARIA USB-Serial-JTAG (sdkconfig del core:
// CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y). Si el monitor está en el puerto USB nativo,
// se ven los logs pero NO lo impreso con Serial. log_printf() usa ets_printf, así que sale por
// donde salen los logs, sea cual sea el puerto.
//   DIAG_VIA_LOG = 1 (defecto): solo log_printf  -> mismo monitor que los logs del stack.
//   DIAG_VIA_LOG = 0          : solo Serial.
//   DIAG_ALSO_SERIAL = 1      : además Serial (duplica líneas si ambos llegan al mismo monitor).
#ifndef DIAG_VIA_LOG
#define DIAG_VIA_LOG 1
#endif
#ifndef DIAG_ALSO_SERIAL
#define DIAG_ALSO_SERIAL 0
#endif

#if DIAG_VIA_LOG && DIAG_ALSO_SERIAL
#define DIAG(fmt, ...) do { log_printf(fmt "\r\n", ##__VA_ARGS__); Serial.printf(fmt "\r\n", ##__VA_ARGS__); } while (0)
#elif DIAG_VIA_LOG
#define DIAG(fmt, ...) log_printf(fmt "\r\n", ##__VA_ARGS__)
#else
#define DIAG(fmt, ...) Serial.printf(fmt "\r\n", ##__VA_ARGS__)
#endif

const uint8_t CANAL_ASIGNADO = 11;  // debe coincidir con EmisorC6 (igual que el ejemplo)

ZigbeeRfidEP zbRfid(rfidzb::COORD_ENDPOINT, /*serverRole=*/true);

// ---------------- Cola callback -> loop ----------------
// Un evento por cada comando recibido en el cluster, válido o no (diagnóstico).
const uint8_t RES_CLUSTER_INESPERADO = 100;  // resto de valores = rfidzb::Decode
const uint8_t RES_COMANDO_INESPERADO = 101;

struct RxEvent {
  uint16_t cluster;
  uint16_t cmdId;
  uint8_t srcEp;
  uint8_t dstEp;
  uint8_t addrType;     // ESP_ZB_ZCL_ADDR_TYPE_*
  uint16_t srcShort;    // válido si addrType == SHORT
  uint8_t srcIeee[8];   // válido si addrType == IEEE
  uint16_t size;        // bytes de payload entregados por el stack
  uint8_t firstByte;
  uint8_t result;       // rfidzb::Decode, o RES_*
  int8_t linkRssi;      // RSSI del enlace Zigbee (distinto del RSSI del lector)
  uint32_t tRxMs;       // millis() del coordinador al ENTREGAR el stack el mensaje (no es la hora de lectura RFID)
  rfidzb::TagMsg msg;   // válido solo si result == Decode::OK
};
QueueHandle_t rxQueue = nullptr;
// Un solo escritor cada uno (callback).
volatile uint32_t cbInvocations = 0;
volatile uint32_t cbQueueFull = 0;

// Callback del stack Zigbee: NO imprimir, NO bloquear, trabajo mínimo (copiar + decodificar ≤ 38 B).
static void onCustomCommand(const esp_zb_zcl_custom_cluster_command_message_t *message) {
  if (message == nullptr || rxQueue == nullptr) return;
  cbInvocations = cbInvocations + 1;

  RxEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.cluster = message->info.cluster;
  ev.cmdId = message->info.command.id;
  ev.srcEp = message->info.src_endpoint;
  ev.dstEp = message->info.dst_endpoint;
  ev.addrType = message->info.src_address.addr_type;
  if (ev.addrType == ESP_ZB_ZCL_ADDR_TYPE_IEEE) {
    memcpy(ev.srcIeee, message->info.src_address.u.ieee_addr, sizeof(ev.srcIeee));
  } else {
    ev.srcShort = message->info.src_address.u.short_addr;
  }
  ev.linkRssi = message->info.header.rssi;
  ev.tRxMs = millis();

  const uint8_t *data = static_cast<const uint8_t *>(message->data.value);
  const size_t size = message->data.size;
  ev.size = static_cast<uint16_t>(size);
  ev.firstByte = (data != nullptr && size > 0) ? data[0] : 0;

  if (ev.cluster != rfidzb::CLUSTER_ID) {
    ev.result = RES_CLUSTER_INESPERADO;
  } else if (ev.cmdId != rfidzb::CMD_TAG_REPORT) {
    ev.result = RES_COMANDO_INESPERADO;
  } else {
    ev.result = static_cast<uint8_t>(rfidzb::decode(data, size, ev.msg));
  }

  if (xQueueSend(rxQueue, &ev, 0) != pdTRUE) cbQueueFull = cbQueueFull + 1;
}

// ---------------- Estado de impresión (solo loop) ----------------
uint32_t totalCallbacks = 0;   // eventos procesados por loop
uint32_t totalValid = 0;       // mensajes RFID válidos recibidos
uint32_t totalRejected = 0;
uint16_t lastId = 0;
bool haveLastId = false;
uint16_t knownSources[4] = {0, 0, 0, 0};
uint8_t knownCount = 0;
unsigned long lastStatus = 0;

static bool isNewSource(uint16_t s) {
  for (uint8_t i = 0; i < knownCount; i++) if (knownSources[i] == s) return false;
  if (knownCount < 4) knownSources[knownCount++] = s;
  return true;
}

// ---------------- Enlace al LilyGO (solo se usa desde loop(), nunca desde el callback) ----------------
// Anillo de eventos válidos pendientes de escribir por UART. La secuencia (seq) se asigna AL ENCOLAR:
// si el anillo se desborda y se descarta el más antiguo, el LilyGO verá el salto de seq.
const size_t LINK_RING = 16;
tracelink::Event linkRing[LINK_RING];
size_t linkHead = 0, linkCount = 0;
uint32_t linkSeq = 0;
uint32_t linkSent = 0, linkAcked = 0, linkDropped = 0, linkBuildErr = 0;
uint32_t lastAckSeq = 0;
unsigned long lastNoAckWarn = 0;
uint8_t coordIeee[8] = {0};  // MSB primero
bool coordIeeeKnown = false;
uint32_t sessionId = 0;

static void ieeeLeToBe(const uint8_t *le, uint8_t *be) { for (int i = 0; i < 8; i++) be[i] = le[7 - i]; }

// IEEE (estable) del emisor a partir de su NWK (puede cambiar al reasociarse). Lock acotado: no bloquea.
static bool lookupIeee(uint16_t nwk, uint8_t *beOut) {
  uint8_t le[8];
  bool ok = false;
  if (esp_zb_lock_acquire(pdMS_TO_TICKS(100))) {
    ok = (esp_zb_ieee_address_by_short(nwk, le) == ESP_OK);
    esp_zb_lock_release();
  }
  if (ok) ieeeLeToBe(le, beOut);
  return ok;
}

static void linkEnqueue(tracelink::Event ev) {
  ev.seq = ++linkSeq;
  if (linkCount == LINK_RING) {  // lleno: descarta el más antiguo (el LilyGO detectará el salto de seq)
    linkHead = (linkHead + 1) % LINK_RING;
    linkCount--;
    linkDropped++;
    DIAG("[LILYGO] Cola pendiente llena: descartado el evento mas antiguo (total descartados: %lu)", (unsigned long)linkDropped);
  }
  linkRing[(linkHead + linkCount) % LINK_RING] = ev;
  linkCount++;
}

// Escribe por UART lo pendiente SOLO si cabe en el TX buffer (nunca espera). Con LilyGO apagado o
// sin cable la UART igualmente "transmite" al vacío: no hay contrapresión, ver ACK más abajo.
static void linkPump() {
  while (linkCount > 0) {
    tracelink::Event ev = linkRing[linkHead];
    ev.tTxMs = millis();
    char line[tracelink::LINE_LEN_MAX + 8];
    const size_t n = tracelink::build(ev, line, sizeof(line));
    if (n == 0) {  // no debería ocurrir; no bloquear la cola por un evento inservible
      linkBuildErr++;
      DIAG("[LILYGO] ERROR: no se pudo serializar el evento seq %lu (descartado)", (unsigned long)ev.seq);
    } else {
      if (Serial1.availableForWrite() < (int)(n + 2)) return;  // sin espacio: reintenta en la próxima vuelta
      Serial1.println(line);  // igual que el ejemplo: línea + "\r\n"
      linkSent++;
      DIAG("[LILYGO] Enviado seq %lu (MSG ID %u, %s) a las %lu ms", (unsigned long)ev.seq, (unsigned)ev.msgId,
           ev.simulated ? "SIMULADO" : "REAL", (unsigned long)ev.tTxMs);
    }
    linkHead = (linkHead + 1) % LINK_RING;
    linkCount--;
  }
}

// ACK opcional del LilyGO ("$ACK,<seq>*HH"). Solo informativo: el C6 funciona igual sin él.
static void linkReadAcks() {
  static char buf[48];
  static size_t len = 0;
  while (Serial1.available()) {
    const char c = static_cast<char>(Serial1.read());
    if (c == '\n') {
      uint32_t s = 0;
      if (len > 0 && tracelink::parseAck(buf, len, s)) {
        linkAcked++;
        lastAckSeq = s;
        DIAG("[LILYGO] ACK seq %lu (LilyGO confirmo recepcion valida)", (unsigned long)s);
      }
      len = 0;
    } else if (c != '\r') {
      if (len < sizeof(buf)) buf[len++] = c; else len = 0;  // basura/línea larga: resincroniza
    }
  }
  // Aviso (como máximo cada 60 s) si se envía y nunca llega ACK.
  if (linkSent >= 3 && linkAcked == 0 && millis() - lastNoAckWarn > 60000UL) {
    lastNoAckWarn = millis();
    DIAG("[LILYGO] Sin ACK tras %lu envios: LilyGO apagado/desconectado, o falta el cable LilyGO GPIO33 -> C6 GPIO11. El coordinador sigue operando.",
         (unsigned long)linkSent);
  }
}

static void printEvent(const RxEvent &ev) {
  totalCallbacks++;

  // --- Diagnóstico de cada comando recibido ---
  DIAG("[DIAG] cmd recibido: cluster=0x%04X cmd=0x%02X src_ep=%u dst_ep=%u size=%u primer_byte=%u",
       ev.cluster, ev.cmdId, ev.srcEp, ev.dstEp, (unsigned)ev.size, (unsigned)ev.firstByte);
  if (ev.addrType == ESP_ZB_ZCL_ADDR_TYPE_IEEE) {
    DIAG("[DIAG] origen IEEE: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
         ev.srcIeee[7], ev.srcIeee[6], ev.srcIeee[5], ev.srcIeee[4], ev.srcIeee[3], ev.srcIeee[2], ev.srcIeee[1], ev.srcIeee[0]);
  } else {
    DIAG("[DIAG] origen NWK: 0x%04X (IEEE no disponible en este mensaje, tipo de direccion %u)", ev.srcShort, ev.addrType);
  }

  if (ev.result == RES_CLUSTER_INESPERADO || ev.result == RES_COMANDO_INESPERADO) {
    totalRejected++;
    DIAG("[DIAG] validacion: RECHAZADO (%s)", ev.result == RES_CLUSTER_INESPERADO ? "cluster inesperado" : "comando inesperado");
    return;
  }
  if (ev.result != static_cast<uint8_t>(rfidzb::Decode::OK)) {
    totalRejected++;
    DIAG("[DIAG] validacion: RECHAZADO (%s, codigo %u)", rfidzb::decodeName(static_cast<rfidzb::Decode>(ev.result)), ev.result);
    return;
  }

  totalValid++;
  DIAG("[DIAG] validacion: OK");

  if (isNewSource(ev.srcShort)) {
    DIAG("[ZIGBEE] Dispositivo incorporado (primer mensaje de 0x%04X)", ev.srcShort);
  }

  char hex[2 * rfidzb::EPC_MAX + 1];
  rfidzb::epcToHex(ev.msg.epc, ev.msg.epcLen, hex, sizeof(hex));

  DIAG("");
  DIAG("[RFID] Mensaje recibido");
  DIAG("[RFID] MSG ID: %u", (unsigned)ev.msg.msgId);
  DIAG("[RFID] EPC: %s (%u bytes)", hex, (unsigned)ev.msg.epcLen);
  if (ev.msg.flags & rfidzb::FLAG_RSSI_VALID) {
    DIAG("[RFID] RSSI: %d dBm (del lector, valor crudo como int8)", (int)ev.msg.rssi);
  } else {
    DIAG("[RFID] RSSI: no disponible en el mensaje");
  }
  DIAG("[RFID] RSSI enlace Zigbee: %d dBm", (int)ev.linkRssi);
  if (ev.msg.flags & rfidzb::FLAG_SIMULATED) {
    DIAG("[RFID] ORIGEN: SIMULADO (emisor 0x%04X)", ev.srcShort);
  } else {
    DIAG("[RFID] ORIGEN: lector RFID real, ESP32-C6 emisor (0x%04X)", ev.srcShort);
  }

  // Pérdidas: id esperado = anterior+1. Id repetido = duplicado. id 1 = el emisor se reinició.
  // (Un único contador global: válido mientras haya un solo emisor.)
  char status = 'V';
  if (haveLastId) {
    if (ev.msg.msgId == lastId) {
      status = 'D';
      DIAG("[RFID] Aviso: MSG ID %u repetido (duplicado)", (unsigned)ev.msg.msgId);
    } else if (ev.msg.msgId != (uint16_t)(lastId + 1) && ev.msg.msgId != 1) {
      status = 'G';
      DIAG("[RFID] Aviso: salto de id (esperaba %u, llego %u): posibles mensajes perdidos",
           (unsigned)(uint16_t)(lastId + 1), (unsigned)ev.msg.msgId);
    }
  }
  lastId = ev.msg.msgId;
  haveLastId = true;

  // ---- Trazabilidad hacia el LilyGO: se conserva MSG ID, EPC, RSSI, origen y tiempos del coordinador ----
  tracelink::Event out{};
  out.msgId = ev.msg.msgId;
  out.simulated = (ev.msg.flags & rfidzb::FLAG_SIMULATED) != 0;
  out.epcLen = ev.msg.epcLen;
  memcpy(out.epc, ev.msg.epc, ev.msg.epcLen);
  out.rssiReader = ev.msg.rssi;
  out.rssiZigbee = ev.linkRssi;
  out.srcNwk = ev.srcShort;
  out.ieeeKnown = lookupIeee(ev.srcShort, out.srcIeee);
  out.coordKnown = coordIeeeKnown;
  memcpy(out.coordIeee, coordIeee, sizeof(coordIeee));
  out.session = sessionId;
  out.tRxMs = ev.tRxMs;
  out.status = status;
  linkEnqueue(out);
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  DIAG("");
  DIAG("=== COORDINADOR RFID ZIGBEE - FIRMWARE PROPIO ===");
  DIAG("=================================");
  DIAG("       RFID ZIGBEE RECEIVER");
  DIAG("=================================");
  DIAG("Reset reason: %d", (int)esp_reset_reason());
#if ARDUINO_USB_CDC_ON_BOOT
  DIAG("[SISTEMA] Serial = USB-CDC (CDC On Boot: Enabled)");
#else
  DIAG("[SISTEMA] Serial = UART0 (CDC On Boot: Disabled)");
#endif
#ifdef CORE_DEBUG_LEVEL
  DIAG("[SISTEMA] Core Debug Level = %d (5=Verbose)", (int)CORE_DEBUG_LEVEL);
#endif
  DIAG("[SISTEMA] Salida DIAG: %s%s", DIAG_VIA_LOG ? "log_printf (canal de logs del stack)" : "Serial",
       (DIAG_VIA_LOG && DIAG_ALSO_SERIAL) ? " + Serial" : "");

  rxQueue = xQueueCreate(16, sizeof(RxEvent));
  if (rxQueue == nullptr) {
    DIAG("[SISTEMA] ERROR: sin memoria para la cola. Reiniciando");
    delay(3000);
    ESP.restart();
  }

  // UART hacia el LilyGO (mismos pines/velocidad que Ejemplos/c6_lilygo/c6). TX buffer propio para que
  // escribir nunca espere a la UART; linkPump() además comprueba availableForWrite().
  Serial1.setTxBufferSize(1024);
  Serial1.begin(LILYGO_BAUD, SERIAL_8N1, LILYGO_RX_PIN, LILYGO_TX_PIN);
  DIAG("[LILYGO] UART1 TX=GPIO%d RX=GPIO%d %d 8N1 (trazabilidad hacia LilyGO; el coordinador funciona sin el)",
       LILYGO_TX_PIN, LILYGO_RX_PIN, LILYGO_BAUD);

  zbRfid.setManufacturerAndModel("UAEH", "RFID_Coord");
  zbRfid.onCustomClusterCommand(onCustomCommand);
  Zigbee.addEndpoint(&zbRfid);

  // Igual que el ejemplo: red abierta 180 s tras cada arranque y canal fijo.
  Zigbee.setRebootOpenNetwork(180);
  Zigbee.setPrimaryChannelMask(1 << CANAL_ASIGNADO);

  DIAG("[ZIGBEE] Creando red en canal %d...", CANAL_ASIGNADO);
  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
    DIAG("[ZIGBEE] ERROR: Zigbee.begin() fallo (revisa Zigbee mode / particion). Reiniciando en 3 s");
    delay(3000);
    ESP.restart();
  }
  DIAG("[ZIGBEE] Coordinador iniciado");

  // Identificadores del coordinador para la trazabilidad (IEEE estable + sesión aleatoria de arranque).
  uint8_t le[8] = {0};
  if (esp_zb_lock_acquire(pdMS_TO_TICKS(500))) {
    esp_zb_get_long_address(le);
    esp_zb_lock_release();
    ieeeLeToBe(le, coordIeee);
    coordIeeeKnown = true;
  }
  sessionId = esp_random();
  DIAG("[SISTEMA] Coordinador IEEE=%02X%02X%02X%02X%02X%02X%02X%02X sesion=%08lX",
       coordIeee[0], coordIeee[1], coordIeee[2], coordIeee[3], coordIeee[4], coordIeee[5], coordIeee[6], coordIeee[7],
       (unsigned long)sessionId);
  DIAG("[ZIGBEE] Red disponible (ventana de union abierta 180 s)");
}

void loop() {
  RxEvent ev;
  while (xQueueReceive(rxQueue, &ev, 0) == pdTRUE) printEvent(ev);
  linkPump();      // escribe al LilyGO lo pendiente (no bloqueante)
  linkReadAcks();  // ACK opcional del LilyGO

  // Estado periódico (uptime + contadores) cada 5 s, para saber que ESTE firmware está corriendo.
  if (millis() - lastStatus >= 5000) {
    lastStatus = millis();
    DIAG("[COORD-FW] uptime=%lus | callbacks=%lu (stack=%lu) | validos=%lu | rechazados=%lu | cola_llena=%lu",
         millis() / 1000, (unsigned long)totalCallbacks, (unsigned long)cbInvocations,
         (unsigned long)totalValid, (unsigned long)totalRejected, (unsigned long)cbQueueFull);
    DIAG("[COORD-FW] LilyGO: enviados=%lu | ack=%lu | pendientes=%u | descartados=%lu | ultimo_ack_seq=%lu",
         (unsigned long)linkSent, (unsigned long)linkAcked, (unsigned)linkCount, (unsigned long)linkDropped, (unsigned long)lastAckSeq);
  }

  delay(20);
}
