// Construye el JSON MQTT de un evento RFID a partir de tracelink::Event (solo campos que ya existen).
// Código puro y constexpr: probado con static_assert en tests/mqtt_json_selftest.cpp.
// Todos los valores son números, hex o enumeraciones fijas: no hay texto libre, así que no se necesita escape.
#pragma once
#include "trace_link.h"

namespace rfidjson {

// Peor caso medido en tests: ver mqtt_json_selftest.cpp (debe caber en MQTT_BUFFER_BYTES - cabecera - tópico).
constexpr size_t JSON_MAX = 400;

namespace detail {
using tracelink::detail::Writer;
constexpr void key(Writer &w, const char *k) { w.ch('"'); w.str(k); w.str("\":"); }
constexpr void strq(Writer &w, const char *k, const char *v) { key(w, k); w.ch('"'); w.str(v); w.ch('"'); }
}  // namespace detail

// Devuelve la longitud (sin NUL) o 0 si no cabe.
constexpr size_t build(const tracelink::Event &e, char *out, size_t cap) {
  using detail::key;
  using detail::strq;
  tracelink::detail::Writer w(out, cap);
  w.ch('{');
  key(w, "msgId");              w.udec(e.msgId);       w.ch(',');
  key(w, "seq");                w.udec(e.seq);         w.ch(',');
  key(w, "epc");                w.ch('"'); w.hexBytes(e.epc, e.epcLen); w.ch('"'); w.ch(',');
  key(w, "rssiReader");         w.dec(e.rssiReader);   w.ch(',');
  key(w, "rssiZigbee");         w.dec(e.rssiZigbee);   w.ch(',');
  strq(w, "origin", e.simulated ? "SIMULADO" : "REAL"); w.ch(',');
  const char st[2] = {e.status, '\0'};
  strq(w, "status", st);        w.ch(',');
  key(w, "senderNwk");          w.str("\"0x"); w.hexN(e.srcNwk, 4); w.ch('"'); w.ch(',');
  key(w, "senderIeee");
  if (e.ieeeKnown) { w.ch('"'); w.hexBytes(e.srcIeee, 8); w.ch('"'); } else { w.str("null"); }
  w.ch(',');
  key(w, "coordinatorIeee");
  if (e.coordKnown) { w.ch('"'); w.hexBytes(e.coordIeee, 8); w.ch('"'); } else { w.str("null"); }
  w.ch(',');
  key(w, "coordinatorSession"); w.ch('"'); w.hexN(e.session, 8); w.ch('"'); w.ch(',');
  key(w, "coordinatorRxUptime"); w.udec(e.tRxMs);      w.ch(',');
  key(w, "coordinatorTxUptime"); w.udec(e.tTxMs);
  w.ch('}');
  w.terminate();
  return w.ok() ? w.len() : 0;
}

}  // namespace rfidjson
