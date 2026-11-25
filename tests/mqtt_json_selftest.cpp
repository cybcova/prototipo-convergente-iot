// Pruebas sin hardware del JSON MQTT y de la configuración (tópico exclusivo, buffer suficiente).
// Se validan en compilación con static_assert (no hay compilador host en este entorno).
#include "../LilyGoReceptor/mqtt_config.h"
#include "../LilyGoReceptor/rfid_json.h"

using namespace tracelink;

// ---------- Utilidades constexpr ----------
constexpr size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
constexpr bool startsWith(const char *s, const char *p) { for (size_t i = 0; p[i]; i++) if (s[i] != p[i]) return false; return true; }
constexpr bool contains(const char *s, const char *sub) {
  for (size_t i = 0; s[i]; i++) { size_t j = 0; while (sub[j] && s[i + j] == sub[j]) j++; if (!sub[j]) return true; }
  return false;
}
constexpr bool equals(const char *a, const char *b) { size_t i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return false; return a[i] == b[i]; }

// ---------- Tópico exclusivo ----------
static_assert(equals(MQTT_TOPIC_EVENTS, "iot/prototipo-convergente/rfid/events"), "topico fijo propuesto");
static_assert(!startsWith(MQTT_TOPIC_EVENTS, "GsmClientTest"), "no reutiliza el prefijo del ejemplo");
static_assert(!contains(MQTT_TOPIC_EVENTS, "potenciometro"), "no reutiliza el sufijo del ejemplo");
static_assert(!contains(MQTT_TOPIC_EVENTS, "+") && !contains(MQTT_TOPIC_EVENTS, "#"), "sin comodines MQTT");
static_assert(!contains(MQTT_TOPIC_EVENTS, " "), "sin espacios");
static_assert(MQTT_TOPIC_EVENTS[0] != '/', "sin barra inicial");

// ---------- Evento de muestra ----------
constexpr Event sample(bool sim, bool ieee, char st) {
  Event e{};
  e.seq = 41; e.msgId = 570; e.simulated = sim; e.epcLen = 12;
  const uint8_t epc[12] = {0xE2, 0x80, 0x68, 0x94, 0x00, 0x00, 0x50, 0x12, 0x34, 0x56, 0x7A, 0xC9};
  for (int i = 0; i < 12; i++) e.epc[i] = epc[i];
  e.rssiReader = -45; e.rssiZigbee = -35; e.srcNwk = 0x1CB3;
  e.ieeeKnown = ieee;
  for (int i = 0; i < 8; i++) e.srcIeee[i] = static_cast<uint8_t>(0x10 + i);
  e.coordKnown = true;
  for (int i = 0; i < 8; i++) e.coordIeee[i] = static_cast<uint8_t>(0xA0 + i);
  e.session = 0xDEADBEEF; e.tRxMs = 577671; e.tTxMs = 577690; e.status = st;
  return e;
}

constexpr bool jsonIs(const Event &e, const char *want) {
  char b[rfidjson::JSON_MAX + 8] = {};
  size_t n = rfidjson::build(e, b, sizeof(b));
  return n != 0 && n == slen(want) && equals(b, want);
}

static_assert(jsonIs(sample(false, true, 'V'),
  "{\"msgId\":570,\"seq\":41,\"epc\":\"E28068940000501234567AC9\",\"rssiReader\":-45,\"rssiZigbee\":-35,"
  "\"origin\":\"REAL\",\"status\":\"V\",\"senderNwk\":\"0x1CB3\",\"senderIeee\":\"1011121314151617\","
  "\"coordinatorIeee\":\"A0A1A2A3A4A5A6A7\",\"coordinatorSession\":\"DEADBEEF\","
  "\"coordinatorRxUptime\":577671,\"coordinatorTxUptime\":577690}"), "JSON exacto REAL");

static_assert(jsonIs(sample(true, false, 'G'),
  "{\"msgId\":570,\"seq\":41,\"epc\":\"E28068940000501234567AC9\",\"rssiReader\":-45,\"rssiZigbee\":-35,"
  "\"origin\":\"SIMULADO\",\"status\":\"G\",\"senderNwk\":\"0x1CB3\",\"senderIeee\":null,"
  "\"coordinatorIeee\":\"A0A1A2A3A4A5A6A7\",\"coordinatorSession\":\"DEADBEEF\","
  "\"coordinatorRxUptime\":577671,\"coordinatorTxUptime\":577690}"), "JSON exacto SIMULADO con IEEE desconocida");

// El JSON es el MISMO dato que la línea UART: ida y vuelta línea -> Event -> JSON conserva los campos
constexpr bool uartToJson() {
  Event in = sample(true, true, 'D');
  char line[LINE_LEN_MAX + 8] = {};
  size_t n = tracelink::build(in, line, sizeof(line));
  Event out{};
  if (n == 0 || tracelink::parse(line, n, out) != Parse::OK) return false;
  char a[rfidjson::JSON_MAX + 8] = {}, b[rfidjson::JSON_MAX + 8] = {};
  size_t la = rfidjson::build(in, a, sizeof(a)), lb = rfidjson::build(out, b, sizeof(b));
  return la != 0 && la == lb && equals(a, b);
}
static_assert(uartToJson(), "UART -> Event -> JSON consistente");

// Peor caso: EPC de 32 B, valores máximos, IEEE conocidas
constexpr size_t worstJson() {
  Event e{}; e.seq = 0xFFFFFFFFu; e.msgId = 0xFFFF; e.simulated = true; e.epcLen = EPC_MAX;
  for (size_t i = 0; i < EPC_MAX; i++) e.epc[i] = 0xFF;
  e.rssiReader = -32768; e.rssiZigbee = -32768; e.srcNwk = 0xFFFF; e.ieeeKnown = true; e.coordKnown = true;
  e.session = 0xFFFFFFFFu; e.tRxMs = 0xFFFFFFFFu; e.tTxMs = 0xFFFFFFFFu; e.status = 'V';
  char b[rfidjson::JSON_MAX + 8] = {};
  return rfidjson::build(e, b, sizeof(b));
}
constexpr size_t WORST = worstJson();
static_assert(WORST != 0 && WORST <= rfidjson::JSON_MAX, "peor caso cabe en JSON_MAX");
// PubSubClient exige: 5 (cabecera) + 2 + len(topico) + len(payload) <= bufferSize
static_assert(5 + 2 + sizeof(MQTT_TOPIC_EVENTS) - 1 + WORST <= MQTT_BUFFER_BYTES, "peor caso cabe en el buffer MQTT configurado");
// ...y el buffer por defecto de PubSubClient (256) NO bastaría: por eso se configura setBufferSize
static_assert(5 + 2 + sizeof(MQTT_TOPIC_EVENTS) - 1 + WORST > 256, "con 256 B (por defecto) no cabría");

// build rechaza buffers pequeños y no escribe fuera
constexpr bool smallBuffer() {
  char b[40] = {};
  return rfidjson::build(sample(false, true, 'V'), b, sizeof(b)) == 0 &&
         rfidjson::build(sample(false, true, 'V'), nullptr, 100) == 0;
}
static_assert(smallBuffer(), "buffer pequeno rechazado");

// El JSON no contiene caracteres que requieran escape (control, comilla o barra invertida dentro de valores)
constexpr bool noEscapesNeeded() {
  char b[rfidjson::JSON_MAX + 8] = {};
  size_t n = rfidjson::build(sample(true, true, 'V'), b, sizeof(b));
  int quotes = 0;
  for (size_t i = 0; i < n; i++) {
    if (b[i] == '\\' || b[i] < 32) return false;
    if (b[i] == '"') quotes++;
  }
  return quotes % 2 == 0;
}
static_assert(noEscapesNeeded(), "sin escapes y comillas balanceadas");

// Reglas de configuración
static_assert(PENDING_QUEUE_LEN >= 8 && PENDING_QUEUE_LEN <= 64, "cola pequeña y razonable");
static_assert(MODEM_UART_RX == 26 && MODEM_UART_TX == 27 && MODEM_PWR_PIN == 4, "pines del módem = ejemplo");
static_assert(MQTT_PORT == 1883, "puerto = ejemplo");

int main() { return 0; }
