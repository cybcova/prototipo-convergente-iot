// Enlace serial Coordinador (ESP32-C6) -> LilyGO 7070: formato de línea con trazabilidad.
// Ver PROTOCOLO_C6_LILYGO.md.
//
// Mecanismo heredado de Ejemplos/c6_lilygo: UART 115200 8N1, texto ASCII, un mensaje por línea
// terminada en '\n' (el C6 usa println => "\r\n"; el receptor ignora '\r'). Se AÑADE a ese
// mecanismo: prefijo '$', checksum XOR '*HH' y número de secuencia, para que el receptor pueda
// validar que la línea está completa y detectar pérdidas/duplicados.
//
// IMPORTANTE: este archivo existe en DOS copias idénticas (CoordinadorC6/ y LilyGoReceptor/)
// porque Arduino solo compila la carpeta del sketch (tests/run_checks.sh verifica que coincidan).
// Código puro (sin Arduino) y constexpr para validarlo con static_assert (tests/).
//
// Línea (todo ASCII, sin espacios):
//   $RFID,<ver>,<seq>,<msgId>,<R|S>,<EPC hex>,<rssiLector>,<rssiZigbee>,<nwk hex4>,<ieee hex16|->,
//   <coordinador ieee hex16|->,<sesion hex8>,<tRxMs>,<tTxMs>,<estado V|D|G>*<HH>
// ACK opcional (LilyGO -> C6):  $ACK,<seq>*<HH>
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tracelink {

constexpr uint8_t VERSION = 1;
constexpr size_t LINE_LEN_MAX = 224;  // sin '\r\n'; máx. real ≈ 190 con EPC de 32 B
constexpr size_t EPC_MAX = 32;

enum class Parse : uint8_t {
  OK = 0,
  EMPTY,
  TOO_LONG,
  NO_START,       // no empieza con '$'
  NO_CHECKSUM,    // falta '*HH' al final (línea truncada o sin checksum)
  BAD_CHECKSUM,   // contenido corrupto
  BAD_FIELDS,     // número de campos incorrecto
  BAD_VERSION,
  BAD_NUMBER,
  BAD_HEX,
  BAD_EPC,        // longitud 0, impar o > EPC_MAX
  BAD_ORIGIN,
  BAD_STATUS
};

struct Event {
  uint32_t seq;          // secuencia del ENLACE serial (asignada por el coordinador al encolar)
  uint16_t msgId;        // MSG ID original del emisor (sin modificar)
  bool simulated;        // origen: false = REAL, true = SIMULADO (FLAG_SIMULATED original)
  uint8_t epcLen;
  uint8_t epc[EPC_MAX];  // EPC completo, tal como llegó
  int16_t rssiReader;    // RSSI del lector (byte crudo como int8)
  int16_t rssiZigbee;    // RSSI del enlace Zigbee en el coordinador
  uint16_t srcNwk;       // dirección NWK del emisor (puede cambiar si se reasocia)
  bool ieeeKnown;
  uint8_t srcIeee[8];    // IEEE del emisor, MSB primero (identificador estable)
  bool coordKnown;
  uint8_t coordIeee[8];  // IEEE del coordinador, MSB primero
  uint32_t session;      // identificador aleatorio de arranque del coordinador
  uint32_t tRxMs;        // uptime del coordinador (ms) al RECIBIR el mensaje Zigbee
  uint32_t tTxMs;        // uptime del coordinador (ms) al escribir la línea por UART
  char status;           // 'V' válido, 'D' duplicado Zigbee (mismo MSG ID repetido), 'G' tras salto de MSG ID
};

namespace detail {

class Writer {
public:
  constexpr Writer(char *b, size_t cap) : b_(b), cap_(cap), n_(0), ok_(b != nullptr && cap > 0) {}
  constexpr void ch(char c) {
    if (ok_ && n_ + 1 < cap_) b_[n_++] = c; else ok_ = false;
  }
  constexpr void str(const char *s) { while (*s) ch(*s++); }
  constexpr void dec(int32_t v) {
    uint32_t m = (v < 0) ? (0u - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
    if (v < 0) ch('-');
    udec(m);
  }
  constexpr void udec(uint32_t m) {
    char t[10] = {};
    size_t k = 0;
    do { t[k++] = static_cast<char>('0' + (m % 10)); m /= 10; } while (m != 0);
    while (k > 0) ch(t[--k]);
  }
  constexpr void hexN(uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) ch(hexc((v >> (4 * i)) & 0xF));
  }
  constexpr void hexBytes(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) hexN(p[i], 2);
  }
  constexpr void terminate() { if (ok_) b_[n_] = '\0'; }
  constexpr size_t len() const { return n_; }
  constexpr bool ok() const { return ok_; }
  constexpr char *data() const { return b_; }
  static constexpr char hexc(uint32_t v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

private:
  char *b_;
  size_t cap_;
  size_t n_;
  bool ok_;
};

constexpr int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

struct Tok { const char *p; size_t n; };

constexpr bool tokIs(const Tok &t, const char *s) {
  size_t i = 0;
  for (; i < t.n; i++) if (s[i] == '\0' || s[i] != t.p[i]) return false;
  return s[i] == '\0';
}

constexpr bool parseU(const Tok &t, uint32_t &out, uint64_t maxv) {
  if (t.n == 0 || t.n > 10) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < t.n; i++) {
    if (t.p[i] < '0' || t.p[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(t.p[i] - '0');
  }
  if (v > maxv) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

constexpr bool parseI(const Tok &t, int32_t &out, int32_t lo, int32_t hi) {
  if (t.n == 0) return false;
  bool neg = (t.p[0] == '-');
  Tok d = {t.p + (neg ? 1 : 0), t.n - (neg ? 1 : 0)};
  uint32_t m = 0;
  if (!parseU(d, m, 1000000u)) return false;
  int32_t v = neg ? -static_cast<int32_t>(m) : static_cast<int32_t>(m);
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

// Hex de exactamente `digits` dígitos -> entero
constexpr bool parseHexFixed(const Tok &t, uint32_t &out, size_t digits) {
  if (t.n != digits) return false;
  uint32_t v = 0;
  for (size_t i = 0; i < t.n; i++) {
    int h = hexVal(t.p[i]);
    if (h < 0) return false;
    v = (v << 4) | static_cast<uint32_t>(h);
  }
  out = v;
  return true;
}

constexpr bool parseHexBytes(const Tok &t, uint8_t *out, size_t bytes) {
  if (t.n != bytes * 2) return false;
  for (size_t i = 0; i < bytes; i++) {
    int h = hexVal(t.p[2 * i]), l = hexVal(t.p[2 * i + 1]);
    if (h < 0 || l < 0) return false;
    out[i] = static_cast<uint8_t>((h << 4) | l);
  }
  return true;
}

// Cierra la trama: '*' + XOR(hex2) sobre lo escrito después de '$'. Requiere que b[0]=='$'.
constexpr size_t finish(Writer &w) {
  uint8_t x = 0;
  for (size_t i = 1; i < w.len(); i++) x ^= static_cast<uint8_t>(w.data()[i]);
  w.ch('*');
  w.hexN(x, 2);
  w.terminate();
  return w.ok() ? w.len() : 0;
}

// Valida '$...*HH' y devuelve el rango de carga útil [1, star). false si no es válida.
constexpr Parse frame(const char *line, size_t n, size_t &star) {
  if (line == nullptr || n == 0) return Parse::EMPTY;
  if (n > LINE_LEN_MAX) return Parse::TOO_LONG;
  if (line[0] != '$') return Parse::NO_START;
  if (n < 5 || line[n - 3] != '*') return Parse::NO_CHECKSUM;
  int h = hexVal(line[n - 2]), l = hexVal(line[n - 1]);
  if (h < 0 || l < 0) return Parse::NO_CHECKSUM;
  uint8_t x = 0;
  for (size_t i = 1; i < n - 3; i++) x ^= static_cast<uint8_t>(line[i]);
  if (x != static_cast<uint8_t>((h << 4) | l)) return Parse::BAD_CHECKSUM;
  star = n - 3;
  return Parse::OK;
}

}  // namespace detail

// Serializa. Devuelve la longitud (sin NUL ni "\r\n") o 0 si no cabe / datos inválidos.
constexpr size_t build(const Event &e, char *out, size_t cap) {
  if (e.epcLen == 0 || e.epcLen > EPC_MAX) return 0;
  if (e.status != 'V' && e.status != 'D' && e.status != 'G') return 0;
  detail::Writer w(out, cap);
  w.ch('$');
  w.str("RFID,");
  w.udec(VERSION);               w.ch(',');
  w.udec(e.seq);                 w.ch(',');
  w.udec(e.msgId);               w.ch(',');
  w.ch(e.simulated ? 'S' : 'R'); w.ch(',');
  w.hexBytes(e.epc, e.epcLen);   w.ch(',');
  w.dec(e.rssiReader);           w.ch(',');
  w.dec(e.rssiZigbee);           w.ch(',');
  w.hexN(e.srcNwk, 4);           w.ch(',');
  if (e.ieeeKnown) w.hexBytes(e.srcIeee, 8); else w.ch('-');
  w.ch(',');
  if (e.coordKnown) w.hexBytes(e.coordIeee, 8); else w.ch('-');
  w.ch(',');
  w.hexN(e.session, 8);          w.ch(',');
  w.udec(e.tRxMs);               w.ch(',');
  w.udec(e.tTxMs);               w.ch(',');
  w.ch(e.status);
  return detail::finish(w);
}

// Valida y decodifica UNA línea (sin '\r'/'\n'). Si no devuelve OK, `e` no es fiable.
constexpr Parse parse(const char *line, size_t n, Event &e) {
  size_t star = 0;
  Parse f = detail::frame(line, n, star);
  if (f != Parse::OK) return f;

  detail::Tok t[16] = {};
  size_t cnt = 0, start = 1;
  for (size_t i = 1; i <= star; i++) {
    if (i == star || line[i] == ',') {
      if (cnt >= 16) return Parse::BAD_FIELDS;
      t[cnt].p = line + start;
      t[cnt].n = i - start;
      cnt++;
      start = i + 1;
    }
  }
  if (cnt != 15 || !detail::tokIs(t[0], "RFID")) return Parse::BAD_FIELDS;

  uint32_t u = 0;
  int32_t s = 0;
  if (!detail::parseU(t[1], u, 255)) return Parse::BAD_NUMBER;
  if (u != VERSION) return Parse::BAD_VERSION;
  if (!detail::parseU(t[2], e.seq, 0xFFFFFFFFull)) return Parse::BAD_NUMBER;
  if (!detail::parseU(t[3], u, 0xFFFF)) return Parse::BAD_NUMBER;
  e.msgId = static_cast<uint16_t>(u);

  if (t[4].n != 1 || (t[4].p[0] != 'R' && t[4].p[0] != 'S')) return Parse::BAD_ORIGIN;
  e.simulated = (t[4].p[0] == 'S');

  if (t[5].n == 0 || (t[5].n % 2) != 0 || t[5].n > 2 * EPC_MAX) return Parse::BAD_EPC;
  e.epcLen = static_cast<uint8_t>(t[5].n / 2);
  for (size_t i = 0; i < EPC_MAX; i++) e.epc[i] = 0;
  if (!detail::parseHexBytes(t[5], e.epc, e.epcLen)) return Parse::BAD_HEX;

  if (!detail::parseI(t[6], s, -32768, 32767)) return Parse::BAD_NUMBER;
  e.rssiReader = static_cast<int16_t>(s);
  if (!detail::parseI(t[7], s, -32768, 32767)) return Parse::BAD_NUMBER;
  e.rssiZigbee = static_cast<int16_t>(s);

  if (!detail::parseHexFixed(t[8], u, 4)) return Parse::BAD_HEX;
  e.srcNwk = static_cast<uint16_t>(u);

  e.ieeeKnown = !detail::tokIs(t[9], "-");
  if (e.ieeeKnown && !detail::parseHexBytes(t[9], e.srcIeee, 8)) return Parse::BAD_HEX;
  e.coordKnown = !detail::tokIs(t[10], "-");
  if (e.coordKnown && !detail::parseHexBytes(t[10], e.coordIeee, 8)) return Parse::BAD_HEX;

  if (!detail::parseHexFixed(t[11], e.session, 8)) return Parse::BAD_HEX;
  if (!detail::parseU(t[12], e.tRxMs, 0xFFFFFFFFull)) return Parse::BAD_NUMBER;
  if (!detail::parseU(t[13], e.tTxMs, 0xFFFFFFFFull)) return Parse::BAD_NUMBER;

  if (t[14].n != 1 || (t[14].p[0] != 'V' && t[14].p[0] != 'D' && t[14].p[0] != 'G')) return Parse::BAD_STATUS;
  e.status = t[14].p[0];
  return Parse::OK;
}

// ACK opcional del LilyGO: "$ACK,<seq>*HH"
constexpr size_t buildAck(uint32_t seq, char *out, size_t cap) {
  detail::Writer w(out, cap);
  w.ch('$');
  w.str("ACK,");
  w.udec(seq);
  return detail::finish(w);
}

constexpr bool parseAck(const char *line, size_t n, uint32_t &seq) {
  size_t star = 0;
  if (detail::frame(line, n, star) != Parse::OK) return false;
  if (star < 6 || !detail::tokIs(detail::Tok{line + 1, 3}, "ACK") || line[4] != ',') return false;
  return detail::parseU(detail::Tok{line + 5, star - 5}, seq, 0xFFFFFFFFull);
}

inline const char *parseName(Parse p) {
  switch (p) {
    case Parse::OK:           return "OK";
    case Parse::EMPTY:        return "linea_vacia";
    case Parse::TOO_LONG:     return "linea_demasiado_larga";
    case Parse::NO_START:     return "sin_inicio_$";
    case Parse::NO_CHECKSUM:  return "sin_checksum(truncada?)";
    case Parse::BAD_CHECKSUM: return "checksum_incorrecto";
    case Parse::BAD_FIELDS:   return "numero_de_campos";
    case Parse::BAD_VERSION:  return "version_desconocida";
    case Parse::BAD_NUMBER:   return "numero_invalido";
    case Parse::BAD_HEX:      return "hex_invalido";
    case Parse::BAD_EPC:      return "epc_invalido";
    case Parse::BAD_ORIGIN:   return "origen_invalido";
    case Parse::BAD_STATUS:   return "estado_invalido";
  }
  return "?";
}

}  // namespace tracelink
