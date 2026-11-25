// Pruebas sin hardware del protocolo RFID/Zigbee y del parser YRM10002.
// Se validan en TIEMPO DE COMPILACIÓN con static_assert: si este archivo compila, pasan.
// No hay host con compilador en este entorno, así que no se ejecuta nada: se compila
// (-fsyntax-only) con el g++ de la toolchain del core esp32. Ver tests/run_checks.sh.
#include "../EmisorC6/rfid_zigbee_protocol.h"
#include "../EmisorC6/yrm_parser.h"

using namespace rfidzb;

// ---------- Protocolo ----------
constexpr TagMsg makeMsg(uint16_t id, int8_t rssi, uint8_t len) {
  TagMsg m{};
  m.msgId = id; m.rssi = rssi; m.flags = FLAG_RSSI_VALID; m.epcLen = len;
  for (size_t i = 0; i < len; i++) m.epc[i] = static_cast<uint8_t>(0xE2 + i);
  return m;
}

// Ida y vuelta: EPC de 12 bytes (96 bits), RSSI negativo, id con wrap
constexpr bool roundTrip(uint16_t id, int8_t rssi, uint8_t len) {
  TagMsg a = makeMsg(id, rssi, len);
  uint8_t buf[WIRE_MAX] = {};
  size_t n = encode(a, buf, sizeof(buf));
  if (n != 1 + HEADER_LEN + len) return false;
  TagMsg b{};
  if (decode(buf, n, b) != Decode::OK) return false;
  if (b.msgId != id || b.rssi != rssi || b.epcLen != len || b.flags != FLAG_RSSI_VALID) return false;
  for (size_t i = 0; i < len; i++) if (b.epc[i] != a.epc[i]) return false;
  return true;
}
static_assert(roundTrip(1, -48, 12), "EPC 12 bytes");
static_assert(roundTrip(0xFFFF, -128, 12), "id maximo, rssi minimo");
static_assert(roundTrip(0x1234, 127, 1), "EPC minimo");
static_assert(roundTrip(7, -1, EPC_MAX), "EPC maximo");

// Bandera de simulación: viaja en flags sin alterar el EPC y es independiente de RSSI válido
constexpr bool simulatedFlag() {
  TagMsg a = makeMsg(9, -45, 12);
  a.flags = FLAG_RSSI_VALID | FLAG_SIMULATED;
  uint8_t w[WIRE_MAX] = {};
  size_t n = encode(a, w, sizeof(w));
  TagMsg b{};
  if (n == 0 || decode(w, n, b) != Decode::OK) return false;
  if (!(b.flags & FLAG_SIMULATED) || !(b.flags & FLAG_RSSI_VALID)) return false;
  for (size_t i = 0; i < 12; i++) if (b.epc[i] != a.epc[i]) return false;
  return FLAG_SIMULATED != FLAG_RSSI_VALID;
}
static_assert(simulatedFlag(), "flag simulado");

// Formato en el cable (little-endian explícito, sin structs binarias)
constexpr bool wireLayout() {
  TagMsg m = makeMsg(0x0102, -48, 2);
  uint8_t b[WIRE_MAX] = {};
  size_t n = encode(m, b, sizeof(b));
  return n == 9 && b[0] == 8 && b[1] == PROTO_VERSION && b[2] == 0x02 && b[3] == 0x01 &&
         b[4] == 0xD0 /* -48 */ && b[5] == FLAG_RSSI_VALID && b[6] == 2 && b[7] == 0xE2 && b[8] == 0xE3;
}
static_assert(wireLayout(), "layout en el cable");

// Rechazos de encode
constexpr bool encodeRejects() {
  uint8_t b[WIRE_MAX] = {};
  TagMsg zero = makeMsg(1, 0, 0);
  TagMsg big = makeMsg(1, 0, 12); big.epcLen = EPC_MAX + 1;
  TagMsg ok = makeMsg(1, 0, 12);
  return encode(zero, b, sizeof(b)) == 0 && encode(big, b, sizeof(b)) == 0 &&
         encode(ok, b, 5) == 0 && encode(ok, nullptr, 50) == 0;
}
static_assert(encodeRejects(), "encode rechaza invalidos");

// Rechazos de decode (tamaños y EPC corruptos)
constexpr Decode decodeOf(const uint8_t *w, size_t n) { TagMsg m{}; return decode(w, n, m); }
constexpr bool decodeRejects() {
  TagMsg a = makeMsg(5, -40, 12);
  uint8_t w[WIRE_MAX] = {};
  size_t n = encode(a, w, sizeof(w));

  if (decodeOf(nullptr, 5) != Decode::NULL_OR_EMPTY) return false;
  if (decodeOf(w, 0) != Decode::NULL_OR_EMPTY) return false;
  if (decodeOf(w, n - 1) != Decode::BAD_PREFIX) return false;       // truncado
  { uint8_t x[WIRE_MAX] = {}; for (size_t i = 0; i < n; i++) x[i] = w[i]; x[1] = 9;
    if (decodeOf(x, n) != Decode::BAD_VERSION) return false; }
  { uint8_t x[WIRE_MAX] = {}; for (size_t i = 0; i < n; i++) x[i] = w[i]; x[6] = 0;
    if (decodeOf(x, n) != Decode::BAD_EPC_LEN) return false; }
  { uint8_t x[WIRE_MAX] = {}; for (size_t i = 0; i < n; i++) x[i] = w[i]; x[6] = EPC_MAX + 1;
    if (decodeOf(x, n) != Decode::BAD_EPC_LEN) return false; }
  { uint8_t x[WIRE_MAX] = {}; for (size_t i = 0; i < n; i++) x[i] = w[i]; x[6] = 11;  // epcLen != cuerpo
    if (decodeOf(x, n) != Decode::LEN_MISMATCH) return false; }
  { uint8_t x[3] = {2, PROTO_VERSION, 0};
    if (decodeOf(x, 3) != Decode::TOO_SHORT) return false; }
  // trailing bytes tras el cuerpo: se aceptan (prefijo manda), sin leer fuera del cuerpo
  { uint8_t x[WIRE_MAX + 4] = {}; for (size_t i = 0; i < n; i++) x[i] = w[i];
    if (decodeOf(x, n + 4) != Decode::OK) return false; }
  return true;
}
static_assert(decodeRejects(), "decode rechaza corruptos");

// ---------- Parser YRM10002 ----------
// Construye una trama BB type cmd lenH lenL data.. checksum 7E
struct Frame { uint8_t b[128]; size_t n; };
constexpr Frame buildFrame(uint8_t type, uint8_t cmd, const uint8_t *data, uint16_t len) {
  Frame f{};
  f.b[0] = 0xBB; f.b[1] = type; f.b[2] = cmd; f.b[3] = len >> 8; f.b[4] = len & 0xFF;
  for (uint16_t i = 0; i < len; i++) f.b[5 + i] = data[i];
  uint8_t cs = 0;
  for (size_t i = 1; i < 5u + len; i++) cs = static_cast<uint8_t>(cs + f.b[i]);
  f.b[5 + len] = cs; f.b[6 + len] = 0x7E; f.n = 7u + len;
  return f;
}

// Inventario: RSSI(1) PC(2) EPC(n) CRC(2). PC=0x3000 -> 6 palabras = 12 bytes.
constexpr Frame inventoryFrame(int8_t rssi, bool corruptChecksum) {
  uint8_t d[1 + 2 + 12 + 2] = {static_cast<uint8_t>(rssi), 0x30, 0x00};
  for (int i = 0; i < 12; i++) d[3 + i] = static_cast<uint8_t>(0xE2 + i);
  d[15] = 0x12; d[16] = 0x34;
  Frame f = buildFrame(0x02, 0x22, d, sizeof(d));
  if (corruptChecksum) f.b[f.n - 2] = static_cast<uint8_t>(f.b[f.n - 2] + 1);
  return f;
}

// Comando de inventario del propio firmware: su checksum es correcto (0x83)
constexpr bool inventoryCmdChecksum() {
  const uint8_t cmd[] = {0xBB, 0x00, 0x27, 0x00, 0x03, 0x22, 0x27, 0x10, 0x83, 0x7E};
  uint8_t cs = 0;
  for (size_t i = 1; i < 8; i++) cs = static_cast<uint8_t>(cs + cmd[i]);
  return cs == cmd[8];
}
static_assert(inventoryCmdChecksum(), "checksum del comando de inventario");

constexpr bool parserTag() {
  yrm::Parser p;
  Frame f = inventoryFrame(-48, false);
  yrm::Event ev = yrm::Event::NONE;
  for (size_t i = 0; i < f.n; i++) {
    ev = p.feed(f.b[i]);
    if (i + 1 < f.n && ev != yrm::Event::NONE) return false;  // sin eventos prematuros
  }
  if (ev != yrm::Event::TAG) return false;
  if (p.tag().epcLen != 12 || p.tag().rssi != -48) return false;
  for (int i = 0; i < 12; i++) if (p.tag().epc[i] != 0xE2 + i) return false;
  return true;
}
static_assert(parserTag(), "parser: trama de inventario valida");

constexpr bool parserNoiseAndResync() {
  yrm::Parser p;
  // Basura previa sin 0xBB se ignora; luego trama válida
  const uint8_t junk[] = {0x00, 0x11, 0x7E, 0xFF};
  for (uint8_t b : junk) if (p.feed(b) != yrm::Event::NONE) return false;
  Frame f = inventoryFrame(-30, false);
  yrm::Event ev = yrm::Event::NONE;
  for (size_t i = 0; i < f.n; i++) ev = p.feed(f.b[i]);
  if (ev != yrm::Event::TAG || p.tag().rssi != -30) return false;
  // Dos tramas seguidas
  Frame g = inventoryFrame(-60, false);
  for (size_t i = 0; i < g.n; i++) ev = p.feed(g.b[i]);
  return ev == yrm::Event::TAG && p.tag().rssi == -60;
}
static_assert(parserNoiseAndResync(), "parser: ruido y trama consecutiva");

constexpr bool parserRejects() {
  yrm::Parser p;
  // checksum incorrecto
  Frame bad = inventoryFrame(-48, true);
  yrm::Event ev = yrm::Event::NONE;
  for (size_t i = 0; i < bad.n; i++) ev = p.feed(bad.b[i]);
  if (ev != yrm::Event::BAD_CHECKSUM) return false;

  // terminador incorrecto
  Frame t = inventoryFrame(-48, false);
  t.b[t.n - 1] = 0x00;
  for (size_t i = 0; i < t.n; i++) ev = p.feed(t.b[i]);
  if (ev != yrm::Event::BAD_FRAME) return false;

  // longitud declarada > buffer (0xFFFF): se descarta sin desbordar y se recupera
  const uint8_t huge[] = {0xBB, 0x02, 0x22, 0xFF, 0xFF};
  for (uint8_t b : huge) p.feed(b);
  Frame ok = inventoryFrame(-10, false);
  for (size_t i = 0; i < ok.n; i++) ev = p.feed(ok.b[i]);
  if (ev != yrm::Event::TAG || p.tag().rssi != -10) return false;

  // PC declara EPC de 62 bytes (31 palabras) pero la trama es corta -> BAD_FRAME, nunca lee fuera
  uint8_t d[1 + 2 + 12 + 2] = {0xD0, 0xF8, 0x00};
  Frame lie = buildFrame(0x02, 0x22, d, sizeof(d));
  for (size_t i = 0; i < lie.n; i++) ev = p.feed(lie.b[i]);
  if (ev != yrm::Event::BAD_FRAME) return false;

  // PC con EPC de 0 palabras -> BAD_FRAME
  uint8_t z[1 + 2 + 12 + 2] = {0xD0, 0x00, 0x00};
  Frame zf = buildFrame(0x02, 0x22, z, sizeof(z));
  for (size_t i = 0; i < zf.n; i++) ev = p.feed(zf.b[i]);
  return ev == yrm::Event::BAD_FRAME;
}
static_assert(parserRejects(), "parser: rechazos");

constexpr bool parserReaderError() {
  yrm::Parser p;
  const uint8_t d[] = {0x15};  // 0x15 = sin etiquetas
  Frame f = buildFrame(0x01, 0xFF, d, 1);
  yrm::Event ev = yrm::Event::NONE;
  for (size_t i = 0; i < f.n; i++) ev = p.feed(f.b[i]);
  return ev == yrm::Event::READER_ERROR && p.errorCode() == 0x15;
}
static_assert(parserReaderError(), "parser: trama de error del lector");

// Coherencia entre constantes duplicadas de ambos módulos
static_assert(yrm::EPC_MAX == rfidzb::EPC_MAX, "EPC_MAX debe coincidir");
// Cabe en un único frame APS sin fragmentación (payload APS útil ~ 80 B tras cabeceras ZCL/NWK/seguridad)
static_assert(WIRE_MAX <= 40, "mensaje debe ser pequeño");

int main() { return 0; }
