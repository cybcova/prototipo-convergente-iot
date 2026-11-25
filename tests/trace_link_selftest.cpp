// Pruebas sin hardware del formato de línea Coordinador -> LilyGO (trace_link.h).
// Se validan en compilación con static_assert (no hay compilador host en este entorno).
#include "../CoordinadorC6/trace_link.h"

using namespace tracelink;

constexpr Event sample(uint8_t epcLen, bool sim, char status) {
  Event e{};
  e.seq = 41; e.msgId = 570; e.simulated = sim; e.epcLen = epcLen;
  for (size_t i = 0; i < epcLen; i++) e.epc[i] = static_cast<uint8_t>(0xE2 + i);
  e.rssiReader = -45; e.rssiZigbee = -35; e.srcNwk = 0x1CB3;
  e.ieeeKnown = true;
  for (int i = 0; i < 8; i++) e.srcIeee[i] = static_cast<uint8_t>(0x10 + i);
  e.coordKnown = true;
  for (int i = 0; i < 8; i++) e.coordIeee[i] = static_cast<uint8_t>(0xA0 + i);
  e.session = 0xDEADBEEF; e.tRxMs = 577671; e.tTxMs = 577690; e.status = status;
  return e;
}

constexpr bool same(const Event &a, const Event &b) {
  if (a.seq != b.seq || a.msgId != b.msgId || a.simulated != b.simulated || a.epcLen != b.epcLen) return false;
  for (size_t i = 0; i < a.epcLen; i++) if (a.epc[i] != b.epc[i]) return false;
  if (a.rssiReader != b.rssiReader || a.rssiZigbee != b.rssiZigbee || a.srcNwk != b.srcNwk) return false;
  if (a.ieeeKnown != b.ieeeKnown || a.coordKnown != b.coordKnown) return false;
  if (a.ieeeKnown) for (int i = 0; i < 8; i++) if (a.srcIeee[i] != b.srcIeee[i]) return false;
  if (a.coordKnown) for (int i = 0; i < 8; i++) if (a.coordIeee[i] != b.coordIeee[i]) return false;
  return a.session == b.session && a.tRxMs == b.tRxMs && a.tTxMs == b.tTxMs && a.status == b.status;
}

constexpr bool roundTrip(const Event &in) {
  char buf[LINE_LEN_MAX + 8] = {};
  size_t n = build(in, buf, sizeof(buf));
  if (n == 0 || n > LINE_LEN_MAX) return false;
  Event out{};
  if (parse(buf, n, out) != Parse::OK) return false;
  return same(in, out);
}
static_assert(roundTrip(sample(12, false, 'V')), "EPC 12 B real");
static_assert(roundTrip(sample(12, true, 'D')), "simulado duplicado");
static_assert(roundTrip(sample(1, false, 'G')), "EPC minimo");
static_assert(roundTrip(sample(EPC_MAX, true, 'V')), "EPC maximo cabe en LINE_LEN_MAX");

// Campos opcionales desconocidos, RSSI extremos, enteros máximos
constexpr bool edgeValues() {
  Event e = sample(12, false, 'V');
  e.ieeeKnown = false; e.coordKnown = false;
  e.rssiReader = -128; e.rssiZigbee = 127;
  e.seq = 0xFFFFFFFFu; e.msgId = 0xFFFF; e.tRxMs = 0xFFFFFFFFu; e.tTxMs = 0; e.srcNwk = 0xFFFF;
  if (!roundTrip(e)) return false;
  e.rssiReader = 0; e.session = 0;
  return roundTrip(e);
}
static_assert(edgeValues(), "valores limite");

// Formato exacto (documentado en PROTOCOLO_C6_LILYGO.md)
constexpr bool exactLine() {
  Event e = sample(2, true, 'V');
  e.ieeeKnown = false; e.coordKnown = false;
  char buf[LINE_LEN_MAX] = {};
  size_t n = build(e, buf, sizeof(buf));
  const char *want = "$RFID,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,577671,577690,V*";
  size_t i = 0;
  for (; want[i]; i++) if (i >= n || buf[i] != want[i]) return false;
  return n == i + 2;  // '*' ya incluido en want; + 2 dígitos hex del checksum
}
static_assert(exactLine(), "layout exacto");

// Detección de corrupción: truncada, byte alterado, sin checksum, basura, campos
constexpr Parse parseStr(const char *s, size_t n) { Event e{}; return parse(s, n, e); }
constexpr bool errors() {
  Event e = sample(12, false, 'V');
  char b[LINE_LEN_MAX + 8] = {};
  size_t n = build(e, b, sizeof(b));

  // truncada (cualquier prefijo propio) nunca es válida
  for (size_t k = 1; k < n; k++) if (parseStr(b, k) == Parse::OK) return false;
  // un byte alterado en cualquier posición de la carga útil -> checksum incorrecto
  for (size_t k = 1; k < n - 3; k++) {
    char c = b[k];
    b[k] = static_cast<char>(c ^ 0x01);
    Parse p = parseStr(b, n);
    b[k] = c;
    if (p == Parse::OK) return false;
  }
  if (parseStr(b, 0) != Parse::EMPTY) return false;
  if (parseStr(nullptr, 5) != Parse::EMPTY) return false;
  if (parseStr(b, LINE_LEN_MAX + 1) != Parse::TOO_LONG) return false;
  char x[LINE_LEN_MAX + 8] = {};
  for (size_t i = 0; i < n; i++) x[i] = b[i];
  x[0] = '#';
  if (parseStr(x, n) != Parse::NO_START) return false;
  // línea sin '*HH'
  if (parseStr(b, n - 3) != Parse::NO_CHECKSUM) return false;
  return true;
}
static_assert(errors(), "rechazo de lineas corruptas/truncadas");

// Líneas bien firmadas pero con contenido inválido (se firma a mano para aislar el motivo)
constexpr size_t sign(char *s, size_t len) {  // s = "$....", añade *HH
  uint8_t x = 0;
  for (size_t i = 1; i < len; i++) x ^= static_cast<uint8_t>(s[i]);
  s[len] = '*';
  s[len + 1] = detail::Writer::hexc((x >> 4) & 0xF);
  s[len + 2] = detail::Writer::hexc(x & 0xF);
  s[len + 3] = 0;
  return len + 3;
}
constexpr size_t cp(char *dst, const char *src) { size_t i = 0; while (src[i]) { dst[i] = src[i]; i++; } return i; }
constexpr Parse signedParse(const char *body) {
  char b[LINE_LEN_MAX + 8] = {};
  size_t l = cp(b, body);
  size_t n = sign(b, l);
  Event e{};
  return parse(b, n, e);
}
static_assert(signedParse("$RFID,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::OK, "base valida");
static_assert(signedParse("$RFID,2,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_VERSION, "version");
static_assert(signedParse("$RFID,1,41,570,X,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_ORIGIN, "origen");
static_assert(signedParse("$RFID,1,41,570,S,E2E,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_EPC, "epc impar");
static_assert(signedParse("$RFID,1,41,570,S,,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_EPC, "epc vacio");
static_assert(signedParse("$RFID,1,41,570,S,E2ZZ,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_HEX, "epc no hex");
static_assert(signedParse("$RFID,1,41,570,S,E2E3,-45,-35,1CB,-,-,DEADBEEF,5,6,V") == Parse::BAD_HEX, "nwk corto");
static_assert(signedParse("$RFID,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,Z") == Parse::BAD_STATUS, "estado");
static_assert(signedParse("$RFID,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6") == Parse::BAD_FIELDS, "faltan campos");
static_assert(signedParse("$RFID,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,V,9") == Parse::BAD_FIELDS, "sobran campos");
static_assert(signedParse("$RFID,1,41,70000,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_NUMBER, "msgId fuera de rango");
static_assert(signedParse("$RFID,1,41,570,S,E2E3,abc,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_NUMBER, "rssi no numerico");
static_assert(signedParse("$HOLA,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,5,6,V") == Parse::BAD_FIELDS, "no es RFID");

// build rechaza datos inválidos y buffers pequeños
constexpr bool buildRejects() {
  char b[LINE_LEN_MAX] = {};
  Event e = sample(12, false, 'V');
  if (build(e, b, 20) != 0) return false;            // no cabe
  if (build(e, nullptr, 100) != 0) return false;
  Event z = sample(12, false, 'V'); z.epcLen = 0;
  if (build(z, b, sizeof(b)) != 0) return false;
  Event big = sample(12, false, 'V'); big.epcLen = EPC_MAX + 1;
  if (build(big, b, sizeof(b)) != 0) return false;
  Event st = sample(12, false, 'X');
  return build(st, b, sizeof(b)) == 0;
}
static_assert(buildRejects(), "build rechaza invalidos");

// ACK
constexpr bool ackOk() {
  char b[40] = {};
  size_t n = buildAck(123456, b, sizeof(b));
  uint32_t s = 0;
  if (n == 0 || !parseAck(b, n, s) || s != 123456) return false;
  if (parseAck(b, n - 1, s)) return false;            // truncado
  char c[40] = {};
  for (size_t i = 0; i < n; i++) c[i] = b[i];
  c[5] = '9';                                         // alterado
  if (parseAck(c, n, s)) return false;
  // una línea RFID no es un ACK
  char r[LINE_LEN_MAX] = {};
  size_t m = build(sample(12, false, 'V'), r, sizeof(r));
  return !parseAck(r, m, s);
}
static_assert(ackOk(), "ack");

int main() { return 0; }
