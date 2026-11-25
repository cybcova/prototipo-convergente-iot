// LilyGO T-SIM7070G: receptor de eventos RFID con trazabilidad enviados por el ESP32-C6 coordinador.
// Ver PROTOCOLO_C6_LILYGO.md.
//
// Mecanismo heredado de Ejemplos/c6_lilygo/lilyGo (sin cambios de pines ni velocidad):
//   Serial   = monitor USB, 115200
//   Serial2  = UART desde el C6, 115200 8N1, RX=GPIO32, TX=GPIO33
// Diferencias respecto al ejemplo (que usaba readStringUntil('\n') bloqueante, sin validar):
//   - Lectura NO bloqueante carácter a carácter, con límite de longitud y timeout de línea incompleta.
//   - Validación completa de cada línea (prefijo, checksum, campos, hex, rangos).
//   - Detección de pérdidas/duplicados por número de secuencia del enlace.
//   - ACK opcional por GPIO33 (TX) hacia el C6 (el C6 no depende de él para funcionar).
//
// Placa en Arduino IDE: el ejemplo no la documenta. Se verificó la compilación con "ESP32 Dev Module"
// (esp32:esp32:esp32, core 3.3.8); la T-SIM7070G usa un ESP32-WROVER, por lo que "ESP32 Wrover Module"
// también debería servir (sin probar).

#include <Arduino.h>
#include "trace_link.h"

#define RX_PIN 32  // desde TX (GPIO10) del C6
#define TX_PIN 33  // hacia RX (GPIO11) del C6 (solo ACK)
#define LINK_BAUD 115200

const unsigned long LINE_TIMEOUT_MS = 1000;   // línea sin '\n' durante este tiempo = incompleta
const unsigned long STATS_EVERY_MS = 30000;
const bool SEND_ACK = true;                   // false = no responder al C6

// ---------------- Estado de recepción ----------------
static char rxBuf[tracelink::LINE_LEN_MAX + 1];
static size_t rxLen = 0;
static bool rxOverflow = false;
static unsigned long lastByteMs = 0;

// ---------------- Contadores / seguimiento de secuencia ----------------
static uint32_t okCount = 0, errCount = 0, gapEvents = 0, lostTotal = 0, dupCount = 0, outOfOrder = 0, incompleteCount = 0;
static uint32_t errByReason[16] = {0};
static bool haveSeq = false;
static uint32_t lastSeq = 0;
static uint32_t lastSession = 0;
static unsigned long lastStats = 0;

static void printHexBytes(const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) Serial.printf("%02X", b[i]);
}

// Muestra una línea defectuosa sin caracteres de control (máx. 70 caracteres).
static void printSanitized(const char *s, size_t n) {
  const size_t m = n < 70 ? n : 70;
  for (size_t i = 0; i < m; i++) Serial.write((s[i] >= 32 && s[i] < 127) ? s[i] : '.');
  if (n > m) Serial.print("...");
}

static void reportError(tracelink::Parse why, const char *line, size_t n) {
  errCount++;
  errByReason[static_cast<uint8_t>(why) & 0x0F]++;
  Serial.printf("\n[ERROR RX] motivo=%s | %u bytes | linea=\"", tracelink::parseName(why), (unsigned)n);
  printSanitized(line, n);
  Serial.println("\"");
}

static void sendAck(uint32_t seq) {
  if (!SEND_ACK) return;
  char ack[40];
  if (tracelink::buildAck(seq, ack, sizeof(ack)) > 0) Serial2.println(ack);
}

static void showEvent(const tracelink::Event &e, const char *linkState) {
  Serial.println();
  Serial.println("[EVENTO RFID]");
  Serial.printf("MSG ID: %u\n", (unsigned)e.msgId);
  Serial.print("EPC: ");
  printHexBytes(e.epc, e.epcLen);
  Serial.printf(" (%u bytes)\n", (unsigned)e.epcLen);
  Serial.printf("RSSI LECTOR: %d\n", (int)e.rssiReader);
  Serial.printf("RSSI ZIGBEE: %d dBm\n", (int)e.rssiZigbee);
  Serial.printf("ORIGEN: %s\n", e.simulated ? "SIMULADO" : "REAL");
  Serial.print("COORDINADOR: ESP32-C6");
  if (e.coordKnown) { Serial.print(" (IEEE "); printHexBytes(e.coordIeee, 8); Serial.print(")"); }
  Serial.printf(" sesion %08lX\n", (unsigned long)e.session);
  Serial.printf("DISPOSITIVO ORIGEN: 0x%04X", e.srcNwk);
  if (e.ieeeKnown) { Serial.print(" (IEEE "); printHexBytes(e.srcIeee, 8); Serial.print(")"); }
  Serial.println();
  Serial.printf("ESTADO ZIGBEE: %s\n",
                e.status == 'V' ? "VALIDO" : e.status == 'D' ? "DUPLICADO (MSG ID repetido en Zigbee)" : "VALIDO TRAS SALTO DE MSG ID (posible perdida en Zigbee)");
  // Tiempos: uptime del COORDINADOR (no hora real; el emisor no informa cuándo leyó la etiqueta).
  Serial.printf("T RECEPCION COORDINADOR: %lu ms (uptime del coordinador)\n", (unsigned long)e.tRxMs);
  Serial.printf("T TRANSMISION SERIAL: %lu ms (uptime del coordinador)\n", (unsigned long)e.tTxMs);
  Serial.printf("T RECEPCION LILYGO: %lu ms (uptime del LilyGO)\n", millis());
  Serial.println();
  Serial.println("[TRANSPORTE SERIAL]");
  Serial.printf("SEQ: %lu\n", (unsigned long)e.seq);
  Serial.printf("ESTADO: %s\n", linkState);
  Serial.println("CHECKSUM: OK");
}

static void handleLine(const char *line, size_t n) {
  tracelink::Event e{};
  const tracelink::Parse p = tracelink::parse(line, n, e);
  if (p != tracelink::Parse::OK) {
    reportError(p, line, n);
    return;  // no se hace ACK de líneas inválidas
  }
  okCount++;

  // Seguimiento de secuencia del enlace (se reinicia si cambia la sesión del coordinador).
  const char *state = "RECIBIDO";
  char stateBuf[64];
  bool resync = false;  // true = primer mensaje o nueva sesión: lastSeq se toma de este mensaje
  if (!haveSeq || e.session != lastSession) {
    resync = true;
    if (haveSeq) Serial.println("\n[ENLACE] Nueva sesion del coordinador (reinicio): se reinicia el seguimiento de secuencia");
    lastSession = e.session;
  } else if (e.seq == lastSeq) {
    dupCount++;
    state = "RECIBIDO (DUPLICADO de enlace: mismo SEQ repetido)";
  } else if (e.seq < lastSeq) {
    outOfOrder++;
    state = "RECIBIDO (FUERA DE ORDEN)";
  } else if (e.seq != lastSeq + 1) {
    const uint32_t lost = e.seq - lastSeq - 1;
    gapEvents++;
    lostTotal += lost;
    snprintf(stateBuf, sizeof(stateBuf), "RECIBIDO (SALTO DE SEQ: faltan %lu mensajes)", (unsigned long)lost);
    state = stateBuf;
  }
  if (resync || e.seq > lastSeq) lastSeq = e.seq;
  haveSeq = true;

  showEvent(e, state);
  sendAck(e.seq);
}

static void pollLink() {
  while (Serial2.available()) {
    const char c = static_cast<char>(Serial2.read());
    lastByteMs = millis();
    if (c == '\n') {
      if (rxOverflow) {
        reportError(tracelink::Parse::TOO_LONG, rxBuf, rxLen);
      } else if (rxLen > 0) {
        handleLine(rxBuf, rxLen);
      }
      rxLen = 0;
      rxOverflow = false;
    } else if (c == '\r') {
      continue;
    } else if (rxOverflow) {
      continue;  // descartar hasta el fin de línea
    } else if (rxLen < tracelink::LINE_LEN_MAX) {
      rxBuf[rxLen++] = c;
    } else {
      rxOverflow = true;  // línea demasiado larga: se descarta entera y se resincroniza en '\n'
    }
  }

  // Línea a medias (cable desconectado, C6 reiniciado a mitad de línea): descartar y resincronizar.
  if ((rxLen > 0 || rxOverflow) && millis() - lastByteMs > LINE_TIMEOUT_MS) {
    incompleteCount++;
    Serial.printf("\n[ERROR RX] motivo=linea_incompleta(timeout %lu ms) | %u bytes descartados | fragmento=\"", LINE_TIMEOUT_MS, (unsigned)rxLen);
    printSanitized(rxBuf, rxLen);
    Serial.println("\"");
    errCount++;
    rxLen = 0;
    rxOverflow = false;
  }
}

void setup() {
  Serial.begin(115200);                       // monitor USB (igual que el ejemplo)
  Serial2.setRxBufferSize(1024);              // antes de begin(): margen para ráfagas
  Serial2.begin(LINK_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);  // UART desde el C6 (igual que el ejemplo)
  delay(500);

  Serial.println();
  Serial.println("====================================");
  Serial.println("       RFID TRACEABILITY");
  Serial.println("====================================");
  Serial.println("LilyGO lista, esperando eventos del coordinador...");
  Serial.printf("[ENLACE] UART2 RX=GPIO%d TX=GPIO%d %d 8N1 | linea max %u | ACK %s\n",
                RX_PIN, TX_PIN, LINK_BAUD, (unsigned)tracelink::LINE_LEN_MAX, SEND_ACK ? "activo" : "desactivado");
  lastByteMs = millis();
  lastStats = millis();
}

void loop() {
  pollLink();

  if (millis() - lastStats >= STATS_EVERY_MS) {
    lastStats = millis();
    Serial.printf("\n[ESTADISTICAS] uptime=%lus | ok=%lu | errores=%lu | incompletas=%lu | saltos=%lu (perdidos=%lu) | duplicados=%lu | fuera_de_orden=%lu\n",
                  millis() / 1000, (unsigned long)okCount, (unsigned long)errCount, (unsigned long)incompleteCount,
                  (unsigned long)gapEvents, (unsigned long)lostTotal, (unsigned long)dupCount, (unsigned long)outOfOrder);
  }
  delay(1);
}
