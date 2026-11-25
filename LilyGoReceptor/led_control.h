// Control de dos LEDs por mensajes MQTT "true"/"false". Lógica pura (sin Arduino) y constexpr:
// probada con static_assert en tests/led_control_selftest.cpp.
//
// Reglas (ver DISENO_MQTT_RFID.md §11):
//   "true"  -> verde ON, rojo OFF, 2 s      "false" -> rojo ON, verde OFF, 2 s
//   Cualquier otro payload se IGNORA (no cambia LEDs ni reinicia el temporizador).
//   Un mensaje válido durante el tiempo activo: cambia al LED correspondiente y REINICIA los 2 s.
//   Nunca están ambos encendidos. Sin delay(): se evalúa con millis() desde loop().
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ledctl {

constexpr int GREEN_PIN = 18;   // GPIO18 -> ánodo del LED verde (HIGH = encendido)
constexpr int RED_PIN = 19;     // GPIO19 -> ánodo del LED rojo   (HIGH = encendido)
constexpr uint32_t ON_MS = 2000;

enum class Cmd : uint8_t { INVALID = 0, TRUE_ = 1, FALSE_ = 2 };

// Comparación EXACTA por longitud y contenido (el payload MQTT no está terminado en NUL).
// Solo "true" o "false" en minúsculas; sin espacios ni saltos de línea.
constexpr Cmd parse(const uint8_t *p, size_t n) {
  if (p == nullptr) return Cmd::INVALID;
  if (n == 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') return Cmd::TRUE_;
  if (n == 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') return Cmd::FALSE_;
  return Cmd::INVALID;
}

class Controller {
public:
  enum class State : uint8_t { OFF = 0, GREEN, RED };

  constexpr Controller() : state_(State::OFF), start_(0) {}

  // Aplica una orden válida (INVALID se ignora y devuelve false sin tocar nada).
  constexpr bool apply(Cmd c, uint32_t nowMs) {
    if (c == Cmd::TRUE_)  { state_ = State::GREEN; start_ = nowMs; return true; }
    if (c == Cmd::FALSE_) { state_ = State::RED;   start_ = nowMs; return true; }
    return false;
  }

  // Apaga cuando se cumplen ON_MS. Aritmética sin signo: correcta al desbordar millis() (~49,7 días).
  // Devuelve true si acaba de apagarse.
  constexpr bool update(uint32_t nowMs) {
    if (state_ != State::OFF && static_cast<uint32_t>(nowMs - start_) >= ON_MS) {
      state_ = State::OFF;
      return true;
    }
    return false;
  }

  constexpr bool greenOn() const { return state_ == State::GREEN; }
  constexpr bool redOn() const { return state_ == State::RED; }
  constexpr State state() const { return state_; }

private:
  State state_;
  uint32_t start_;
};

}  // namespace ledctl
