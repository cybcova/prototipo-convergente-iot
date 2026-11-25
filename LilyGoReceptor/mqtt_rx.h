// Clasificación de payloads MQTT recibidos (sin asumir que son texto ni JSON).
// Código puro y constexpr: probado con static_assert en tests/mqtt_rx_selftest.cpp.
// No interpreta ni valida el JSON: solo decide cómo mostrarlo con seguridad en el monitor serial.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mqttrx {

enum class Kind : uint8_t {
  EMPTY = 0,       // longitud 0
  TEXT,            // UTF-8 válido sin caracteres de control (salvo \t \r \n)
  JSON_LIKE,       // TEXT que empieza con { o [ y termina con } o ] (heurística, NO es validación de JSON)
  BINARY           // cualquier otra cosa: UTF-8 inválido, NUL u otros controles
};

// UTF-8 estricto: rechaza secuencias incompletas, continuaciones sueltas, sobrelargas (C0/C1, E0 80..9F,
// F0 80..8F), sustitutos (ED A0..BF) y > U+10FFFF (F4 90.., F5..FF).
constexpr bool validUtf8(const uint8_t *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t b = p[i];
    if (b < 0x80) { i++; continue; }
    size_t need = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) { need = 1; }
    else if (b == 0xE0) { need = 2; lo = 0xA0; }
    else if (b >= 0xE1 && b <= 0xEC) { need = 2; }
    else if (b == 0xED) { need = 2; hi = 0x9F; }
    else if (b >= 0xEE && b <= 0xEF) { need = 2; }
    else if (b == 0xF0) { need = 3; lo = 0x90; }
    else if (b >= 0xF1 && b <= 0xF3) { need = 3; }
    else if (b == 0xF4) { need = 3; hi = 0x8F; }
    else return false;                       // 0x80..0xC1, 0xF5..0xFF
    if (i + need >= n) return false;         // secuencia incompleta: faltan bytes de continuación
    for (size_t k = 1; k <= need; k++) {
      const uint8_t c = p[i + k];
      const uint8_t l = (k == 1) ? lo : 0x80;
      const uint8_t h = (k == 1) ? hi : 0xBF;
      if (c < l || c > h) return false;
    }
    i += need + 1;
  }
  return true;
}

constexpr bool isSpace(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

constexpr Kind classify(const uint8_t *p, size_t n) {
  if (p == nullptr || n == 0) return Kind::EMPTY;
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = p[i];
    if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') return Kind::BINARY;  // incluye NUL
    if (c == 0x7F) return Kind::BINARY;
  }
  if (!validUtf8(p, n)) return Kind::BINARY;
  size_t a = 0, b = n;
  while (a < b && isSpace(p[a])) a++;
  while (b > a && isSpace(p[b - 1])) b--;
  if (b > a) {
    const uint8_t f = p[a], l = p[b - 1];
    if ((f == '{' && l == '}') || (f == '[' && l == ']')) return Kind::JSON_LIKE;
  }
  return Kind::TEXT;
}

inline const char *kindName(Kind k) {
  switch (k) {
    case Kind::EMPTY:     return "VACIO";
    case Kind::TEXT:      return "TEXTO (no parece JSON)";
    case Kind::JSON_LIKE: return "TEXTO (parece JSON, sin validar)";
    case Kind::BINARY:    return "BINARIO / no es texto UTF-8";
  }
  return "?";
}

}  // namespace mqttrx
