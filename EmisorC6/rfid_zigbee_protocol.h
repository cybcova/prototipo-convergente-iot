// Protocolo de aplicación RFID-sobre-Zigbee (v1). Ver PROTOCOLO_RFID_ZIGBEE.md.
//
// IMPORTANTE: este archivo existe en DOS copias idénticas (EmisorC6/ y CoordinadorC6/)
// porque Arduino solo compila los archivos de la carpeta del sketch. Si editas una,
// copia la otra (tests/run_checks.sh verifica que sean idénticas).
//
// Código puro (sin dependencias de Arduino/Zigbee) y constexpr-compatible para poder
// validarlo con static_assert en tests/protocol_selftest.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rfidzb {

constexpr uint16_t CLUSTER_ID = 0xFC10;       // rango manufacturer-specific (0xFC00-0xFFFF)
constexpr uint16_t CMD_TAG_REPORT = 0x01;     // emisor -> coordinador
constexpr uint8_t PROTO_VERSION = 1;
constexpr uint8_t COORD_ENDPOINT = 1;         // mismo EP que el coordinador del ejemplo
constexpr uint8_t SENDER_ENDPOINT = 10;       // mismo EP que el actuador del ejemplo

constexpr size_t EPC_MAX = 32;                // bytes; EPC típico = 12 (96 bits)
constexpr size_t HEADER_LEN = 6;              // ver, id(2), rssi, flags, epcLen
constexpr size_t BODY_MAX = HEADER_LEN + EPC_MAX;
constexpr size_t WIRE_MAX = 1 + BODY_MAX;     // + byte de longitud (octet string ZCL)

constexpr uint8_t FLAG_RSSI_VALID = 0x01;
constexpr uint8_t FLAG_SIMULATED = 0x02;     // dato ficticio de prueba (EmisorC6 con RFID_SIMULATION_MODE)

struct TagMsg {
  uint16_t msgId;
  int8_t rssi;       // valor crudo del lector interpretado como int8 (unidad no verificada)
  uint8_t flags;
  uint8_t epcLen;
  uint8_t epc[EPC_MAX];
};

enum class Decode : uint8_t {
  OK = 0,
  NULL_OR_EMPTY,
  BAD_PREFIX,      // el byte de longitud no cabe en lo recibido
  TOO_SHORT,       // cuerpo menor que la cabecera
  BAD_VERSION,
  BAD_EPC_LEN,     // 0 o > EPC_MAX
  LEN_MISMATCH     // cuerpo != HEADER_LEN + epcLen
};

// Serializa en `out` (octet string ZCL: [len][body]). Devuelve bytes escritos, 0 si inválido.
constexpr size_t encode(const TagMsg &m, uint8_t *out, size_t cap) {
  if (out == nullptr || m.epcLen == 0 || m.epcLen > EPC_MAX) return 0;
  const size_t body = HEADER_LEN + m.epcLen;
  if (cap < 1 + body) return 0;
  out[0] = static_cast<uint8_t>(body);
  out[1] = PROTO_VERSION;
  out[2] = static_cast<uint8_t>(m.msgId & 0xFF);  // little-endian explícito
  out[3] = static_cast<uint8_t>(m.msgId >> 8);
  out[4] = static_cast<uint8_t>(m.rssi);
  out[5] = m.flags;
  out[6] = m.epcLen;
  for (size_t i = 0; i < m.epcLen; i++) out[7 + i] = m.epc[i];
  return 1 + body;
}

// Valida y decodifica. `wireLen` = bytes realmente disponibles en `wire`.
constexpr Decode decode(const uint8_t *wire, size_t wireLen, TagMsg &m) {
  if (wire == nullptr || wireLen == 0) return Decode::NULL_OR_EMPTY;
  const size_t body = wire[0];
  if (body + 1 > wireLen) return Decode::BAD_PREFIX;
  if (body < HEADER_LEN) return Decode::TOO_SHORT;
  if (wire[1] != PROTO_VERSION) return Decode::BAD_VERSION;
  const uint8_t epcLen = wire[6];
  if (epcLen == 0 || epcLen > EPC_MAX) return Decode::BAD_EPC_LEN;
  if (body != HEADER_LEN + epcLen) return Decode::LEN_MISMATCH;
  m.msgId = static_cast<uint16_t>(wire[2] | (static_cast<uint16_t>(wire[3]) << 8));
  m.rssi = static_cast<int8_t>(wire[4]);
  m.flags = wire[5];
  m.epcLen = epcLen;
  for (size_t i = 0; i < EPC_MAX; i++) m.epc[i] = (i < epcLen) ? wire[7 + i] : 0;
  return Decode::OK;
}

// EPC -> hex mayúsculas terminado en '\0'. `cap` debe ser >= 2*len+1; si no, trunca.
inline void epcToHex(const uint8_t *epc, size_t len, char *out, size_t cap) {
  static const char H[] = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; i < len && o + 2 < cap; i++) {
    out[o++] = H[epc[i] >> 4];
    out[o++] = H[epc[i] & 0x0F];
  }
  if (cap > 0) out[o] = '\0';
}

inline const char *decodeName(Decode d) {
  switch (d) {
    case Decode::OK:            return "OK";
    case Decode::NULL_OR_EMPTY: return "vacio";
    case Decode::BAD_PREFIX:    return "longitud_no_cabe";
    case Decode::TOO_SHORT:     return "cuerpo_corto";
    case Decode::BAD_VERSION:   return "version_desconocida";
    case Decode::BAD_EPC_LEN:   return "epc_len_invalida";
    case Decode::LEN_MISMATCH:  return "longitud_inconsistente";
  }
  return "?";
}

}  // namespace rfidzb
