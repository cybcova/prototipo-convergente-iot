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
//
// MQTT (nuevo): cada evento válido se publica por red celular en un tópico fijo y exclusivo
// (mqtt_config.h), replicando la conexión de Ejemplos/lilygoTelcel (TinyGSM SIM7070 + PubSubClient).
// Ver DISENO_MQTT_RFID.md. Reparto de trabajo:
//   loop()     -> UART del coordinador (Serial2): recibe, valida, muestra, ENCOLA y responde ACK. Nunca espera al módem.
//   mqttTask   -> ÚNICO que toca el módem (Serial1) y MQTT: enciende/inicializa, conecta/reconecta y publica la cola.
// SEMÁNTICA: el ACK al coordinador significa "línea UART recibida y válida". NO significa publicado en MQTT.
//            publish() de PubSubClient (QoS 0) devuelve true al escribir en el socket; tampoco es ACK del broker.

#define TINY_GSM_MODEM_SIM7070       // igual que el ejemplo
#define SerialAT Serial1             // UART del módem (GPIO26/27), igual que el ejemplo

#include <Arduino.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include "trace_link.h"
#include "mqtt_config.h"
#include "rfid_json.h"

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

// =====================================================================================
// MQTT por red celular (réplica de la secuencia de Ejemplos/lilygoTelcel, en su propia tarea)
// =====================================================================================
static TinyGsm modem(SerialAT);
static TinyGsmClient gsmClient(modem);
static PubSubClient mqtt(gsmClient);

// Estado visible desde loop() (escrito solo por mqttTask; loop() NUNCA toca modem/mqtt).
enum MqttPhase : uint8_t { PH_INIT = 0, PH_NO_NETWORK, PH_NO_DATA, PH_NO_MQTT, PH_READY };
static volatile uint8_t mqttPhase = PH_INIT;
static volatile uint32_t mqttPublished = 0, mqttPublishFail = 0, mqttGaveUp = 0, mqttBuildErr = 0;
static volatile uint32_t pendDropped = 0, mqttDupSkipped = 0;
static String imei = "";
static String tipoDeRedActual;

// ---- Cola de pendientes en RAM (anillo con mutex; sin persistencia) ----
// Productor: loop() (UART). Consumidor: mqttTask. Sección crítica = solo copiar un Event.
static tracelink::Event pend[PENDING_QUEUE_LEN];
static size_t pHead = 0, pCount = 0;
static SemaphoreHandle_t pMutex = nullptr;

// Lleno: descarta el MÁS ANTIGUO (registrando su seq y MSG ID) para no bloquear nunca la recepción UART.
static void pendPush(const tracelink::Event &e) {
  uint32_t dropSeq = 0;
  uint16_t dropMsg = 0;
  bool dropped = false;
  xSemaphoreTake(pMutex, portMAX_DELAY);
  if (pCount == PENDING_QUEUE_LEN) {
    dropSeq = pend[pHead].seq;
    dropMsg = pend[pHead].msgId;
    dropped = true;
    pHead = (pHead + 1) % PENDING_QUEUE_LEN;
    pCount--;
    pendDropped = pendDropped + 1;
  }
  pend[(pHead + pCount) % PENDING_QUEUE_LEN] = e;
  pCount++;
  xSemaphoreGive(pMutex);
  if (dropped) {
    Serial.printf("[MQTT] Cola de pendientes llena: descartado el mas antiguo (seq %lu, MSG ID %u) sin publicar\n",
                  (unsigned long)dropSeq, (unsigned)dropMsg);
  }
}

static bool pendPeek(tracelink::Event &e) {
  xSemaphoreTake(pMutex, portMAX_DELAY);
  const bool any = pCount > 0;
  if (any) e = pend[pHead];
  xSemaphoreGive(pMutex);
  return any;
}

// Quita la cabeza SOLO si sigue siendo el mismo evento que se publicó (el productor pudo haberla descartado).
static void pendPopIfHead(const tracelink::Event &e) {
  xSemaphoreTake(pMutex, portMAX_DELAY);
  if (pCount > 0 && pend[pHead].seq == e.seq && pend[pHead].session == e.session) {
    pHead = (pHead + 1) % PENDING_QUEUE_LEN;
    pCount--;
  }
  xSemaphoreGive(pMutex);
}

static size_t pendCount() {
  xSemaphoreTake(pMutex, portMAX_DELAY);
  const size_t c = pCount;
  xSemaphoreGive(pMutex);
  return c;
}

// ---- Copia fiel de identificarTecnologiaRed() del ejemplo ----
static void identificarTecnologiaRed() {
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
    Serial.println("[MQTT] Red: " + tipoDeRedActual);
  } else {
    modem.waitResponse();
    Serial.println("[MQTT] No se pudo leer la tecnologia de red.");
  }
}

// ---- Igual que mqttConnect() del ejemplo: Client ID = IMEI, sin usuario/clave; se añade el código de estado al log ----
static bool mqttConnect() {
  Serial.print("[MQTT] Conectando a MQTT...");
  if (!mqtt.connect(imei.c_str())) {
    Serial.printf(" fallo (estado %d)\n", mqtt.state());
    return false;
  }
  Serial.println(" exito!");
  return true;
}

// Publica lo pendiente en orden. Un evento solo sale de la cola cuando publish() devolvió true.
// true de publish() = bytes escritos en el socket (QoS 0): NO es confirmación del broker.
static void publishPending() {
  static uint32_t failSeq = 0, failSession = 0;
  static uint8_t failCount = 0;
  tracelink::Event e;
  while (mqtt.connected() && pendPeek(e)) {
    char json[rfidjson::JSON_MAX + 8];
    const size_t n = rfidjson::build(e, json, sizeof(json));
    if (n == 0) {  // no debería ocurrir; no bloquear la cola por un evento inservible
      mqttBuildErr = mqttBuildErr + 1;
      Serial.printf("[MQTT] ERROR: no se pudo construir el JSON de seq %lu (descartado)\n", (unsigned long)e.seq);
      pendPopIfHead(e);
      continue;
    }
    // retained=false (el ejemplo usaba true para un valor "actual"; un flujo de eventos no debe retenerse).
    if (mqtt.publish(MQTT_TOPIC_EVENTS, json, false)) {
      mqttPublished = mqttPublished + 1;
      failCount = 0;
      pendPopIfHead(e);
      Serial.printf("[MQTT] Publicado seq %lu (MSG ID %u) -> %s\n", (unsigned long)e.seq, (unsigned)e.msgId, MQTT_TOPIC_EVENTS);
      Serial.println(json);
    } else {
      mqttPublishFail = mqttPublishFail + 1;
      if (!mqtt.connected()) return;  // se cayó la conexión: el evento se conserva; reconecta el bucle principal
      // Conectado y aun así falla: reintenta; tras PUBLISH_MAX_ATTEMPTS se descarta para no atascar la cola.
      if (failSeq == e.seq && failSession == e.session) failCount++; else { failSeq = e.seq; failSession = e.session; failCount = 1; }
      if (failCount >= PUBLISH_MAX_ATTEMPTS) {
        mqttGaveUp = mqttGaveUp + 1;
        Serial.printf("[MQTT] publish fallo %u veces con la conexion activa: se descarta seq %lu (MSG ID %u)\n",
                      (unsigned)failCount, (unsigned long)e.seq, (unsigned)e.msgId);
        pendPopIfHead(e);
        failCount = 0;
      } else {
        return;
      }
    }
  }
}

// Secuencia de encendido/inicialización del ejemplo (setup()) + bucle de conexión/reconexión (loop()),
// movidos a esta tarea para que NINGUNA espera del módem detenga la recepción UART.
static void mqttTask(void *) {
  // --- Encendido del módem (igual que el ejemplo) ---
  pinMode(MODEM_PWR_PIN, OUTPUT);
  digitalWrite(MODEM_PWR_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(MODEM_POWER_PULSE_MS));
  digitalWrite(MODEM_PWR_PIN, LOW);

  Serial.println("[MQTT] Esperando a que el modem inicie...");
  SerialAT.begin(MODEM_BAUD, SERIAL_8N1, MODEM_UART_RX, MODEM_UART_TX);
  vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_WAIT_MS));

  modem.restart();
  imei = modem.getIMEI();
  Serial.println("[MQTT] IMEI: " + imei);
  if (imei.length() == 0) Serial.println("[MQTT] AVISO: IMEI vacio (el Client ID sera vacio); revisar el modem");

  modem.setNetworkMode(2);    // Automático
  modem.setPreferredMode(3);  // LTE-M > NB-IoT > GSM

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  if (!mqtt.setBufferSize(MQTT_BUFFER_BYTES)) Serial.println("[MQTT] ERROR: setBufferSize fallo; las publicaciones fallaran");
  // La red, los datos y MQTT se conectan/reconectan en el bucle, como en el ejemplo.

  tipoDeRedActual = modem.getOperator();

  uint32_t lastReconnectAttempt = 0;
  for (;;) {
    // 1) Red celular
    if (!modem.isNetworkConnected()) {
      mqttPhase = PH_NO_NETWORK;
      Serial.println("[MQTT] Buscando red Telcel...");
      if (!modem.waitForNetwork(NETWORK_WAIT_MS, true)) continue;
      identificarTecnologiaRed();
    }

    // 2) Datos (IP)
    if (!modem.isGprsConnected()) {
      mqttPhase = PH_NO_DATA;
      Serial.print("[MQTT] Conectando al APN...");
      if (!modem.gprsConnect(MQTT_APN, MQTT_GPRS_USER, MQTT_GPRS_PASS)) {
        Serial.println(" fallo");
        vTaskDelay(pdMS_TO_TICKS(GPRS_RETRY_PAUSE_MS));
        continue;
      }
      Serial.print(" IP asignada! : ");
      Serial.println(modem.localIP());
    }

    // 3) MQTT
    if (!mqtt.connected()) {
      mqttPhase = PH_NO_MQTT;
      uint32_t t = millis();
      if (t - lastReconnectAttempt > MQTT_RETRY_MS) {
        lastReconnectAttempt = t;
        if (mqttConnect()) lastReconnectAttempt = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    mqttPhase = PH_READY;
    mqtt.loop();
    publishPending();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static const char *phaseName(uint8_t p) {
  switch (p) {
    case PH_INIT:       return "iniciando modem";
    case PH_NO_NETWORK: return "sin red celular";
    case PH_NO_DATA:    return "sin datos (APN)";
    case PH_NO_MQTT:    return "sin MQTT";
    case PH_READY:      return "MQTT conectado";
  }
  return "?";
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
  bool linkDuplicate = false;  // mismo SEQ repetido: ya se encoló/publicó, no se vuelve a publicar
  if (!haveSeq || e.session != lastSession) {
    resync = true;
    if (haveSeq) Serial.println("\n[ENLACE] Nueva sesion del coordinador (reinicio): se reinicia el seguimiento de secuencia");
    lastSession = e.session;
  } else if (e.seq == lastSeq) {
    dupCount++;
    linkDuplicate = true;
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

  // Publicación MQTT: aquí solo se ENCOLA (no bloquea); mqttTask publica cuando hay conexión.
  if (linkDuplicate) {
    mqttDupSkipped = mqttDupSkipped + 1;
    Serial.println("[MQTT] Duplicado de enlace: no se vuelve a publicar");
  } else {
    pendPush(e);
    Serial.printf("[MQTT] Evento encolado para publicar (pendientes: %u | estado: %s)\n",
                  (unsigned)pendCount(), phaseName(mqttPhase));
  }

  // ACK al coordinador = "linea UART recibida y valida". Independiente de MQTT: se envía ya, sin esperar publicación.
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

  // MQTT en su propia tarea: toda espera del módem/red queda fuera de loop() (UART).
  pMutex = xSemaphoreCreateMutex();
  if (pMutex == nullptr) {
    Serial.println("[MQTT] ERROR: sin memoria para el mutex; MQTT desactivado");
    return;
  }
  Serial.printf("[MQTT] Topico fijo: %s | broker %s:%u | cola pendientes %u\n",
                MQTT_TOPIC_EVENTS, MQTT_BROKER, (unsigned)MQTT_PORT, (unsigned)PENDING_QUEUE_LEN);
  // Core 1 (el mismo donde corría loopTask en el ejemplo). NO usar el core 0: este core tiene activo el watchdog de
  // la tarea idle (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y, 5 s, panic) y TinyGSM espera con delay(0), que no cede
  // a la tarea idle: una espera larga del módem (restart/gprsConnect) reiniciaría la placa.
  xTaskCreatePinnedToCore(mqttTask, "mqtt", 10240, nullptr, 1, nullptr, 1);
}

void loop() {
  pollLink();

  if (millis() - lastStats >= STATS_EVERY_MS) {
    lastStats = millis();
    Serial.printf("\n[ESTADISTICAS] uptime=%lus | ok=%lu | errores=%lu | incompletas=%lu | saltos=%lu (perdidos=%lu) | duplicados=%lu | fuera_de_orden=%lu\n",
                  millis() / 1000, (unsigned long)okCount, (unsigned long)errCount, (unsigned long)incompleteCount,
                  (unsigned long)gapEvents, (unsigned long)lostTotal, (unsigned long)dupCount, (unsigned long)outOfOrder);
    if (pMutex != nullptr) {
      Serial.printf("[ESTADISTICAS MQTT] estado=%s | publicados=%lu | pendientes=%u | descartados_cola=%lu | fallos_publish=%lu | abandonados=%lu | dup_enlace_no_publicados=%lu\n",
                    phaseName(mqttPhase), (unsigned long)mqttPublished, (unsigned)pendCount(), (unsigned long)pendDropped,
                    (unsigned long)mqttPublishFail, (unsigned long)mqttGaveUp, (unsigned long)mqttDupSkipped);
    }
  }
  delay(1);
}
