// Pruebas sin hardware de la lógica de los LEDs (led_control.h). Se validan con static_assert.
#include "../LilyGoReceptor/led_control.h"

using namespace ledctl;

template <size_t N> constexpr Cmd P(const char (&s)[N]) {   // literal C: sin el NUL terminal
  uint8_t b[16] = {};
  for (size_t i = 0; i + 1 < N; i++) b[i] = static_cast<uint8_t>(s[i]);
  return parse(b, N - 1);
}

// ---- Parseo exacto (longitud y contenido) ----
static_assert(P("true") == Cmd::TRUE_, "true");
static_assert(P("false") == Cmd::FALSE_, "false");
static_assert(P("TRUE") == Cmd::INVALID, "mayusculas no");
static_assert(P("True") == Cmd::INVALID, "capitalizado no");
static_assert(P("tru") == Cmd::INVALID, "prefijo de true");
static_assert(P("truee") == Cmd::INVALID, "true con un caracter extra");
static_assert(P("fals") == Cmd::INVALID, "prefijo de false");
static_assert(P("falsee") == Cmd::INVALID, "false con un caracter extra");
static_assert(P("true\n") == Cmd::INVALID, "con salto de linea no (estricto)");
static_assert(P(" true") == Cmd::INVALID, "con espacio no (estricto)");
static_assert(P("1") == Cmd::INVALID && P("0") == Cmd::INVALID && P("on") == Cmd::INVALID, "otros valores");
static_assert(P("{\"v\":true}") == Cmd::INVALID, "JSON no");
static_assert(P("") == Cmd::INVALID, "vacio");
static_assert(parse(nullptr, 4) == Cmd::INVALID, "puntero nulo");

// El payload NO está terminado en NUL: "true" seguido de basura fuera de la longitud se acepta; dentro, no.
constexpr bool noNulAssumption() {
  const uint8_t a[8] = {'t', 'r', 'u', 'e', 'X', 'Y', 'Z', 'W'};   // longitud 4: solo "true"
  const uint8_t b[8] = {'t', 'r', 'u', 'e', 0, 0, 0, 0};           // longitud 5 con NUL embebido: inválido
  const uint8_t c[8] = {'f', 'a', 'l', 's', 'e', 'X', 0, 0};       // longitud 6: inválido
  return parse(a, 4) == Cmd::TRUE_ && parse(b, 5) == Cmd::INVALID && parse(c, 6) == Cmd::INVALID && parse(c, 5) == Cmd::FALSE_;
}
static_assert(noNulAssumption(), "longitud manda, no el NUL");

// ---- Arranque: ambos apagados ----
static_assert(!Controller().greenOn() && !Controller().redOn(), "inicio apagado");

// ---- true / false ----
constexpr bool basic() {
  Controller c;
  if (!c.apply(Cmd::TRUE_, 1000) || !c.greenOn() || c.redOn()) return false;
  Controller d;
  return d.apply(Cmd::FALSE_, 1000) && d.redOn() && !d.greenOn();
}
static_assert(basic(), "true->verde, false->rojo, exclusivos");

// ---- 2 segundos exactos ----
constexpr bool timing() {
  Controller c;
  c.apply(Cmd::TRUE_, 5000);
  if (c.update(5000) || !c.greenOn()) return false;       // t=0
  if (c.update(6999) || !c.greenOn()) return false;       // t=1999 sigue encendido
  if (!c.update(7000) || c.greenOn() || c.redOn()) return false;  // t=2000 se apaga, ambos OFF
  if (c.update(9000)) return false;                        // ya apagado: no vuelve a "apagarse"
  return true;
}
static_assert(timing(), "apaga a los 2000 ms");

// ---- Nuevo mensaje válido: cambia de LED y REINICIA el contador ----
constexpr bool restart() {
  Controller c;
  c.apply(Cmd::TRUE_, 0);                 // verde desde t=0
  c.update(1500);                         // aún verde
  c.apply(Cmd::FALSE_, 1500);             // llega false a t=1500: rojo, verde apagado, reinicia
  if (!c.redOn() || c.greenOn()) return false;
  if (c.update(3499) || !c.redOn()) return false;   // 1999 ms desde el reinicio: sigue
  if (!c.update(3500) || c.redOn()) return false;   // 2000 ms desde el reinicio: se apaga
  // mismo valor repetido también reinicia
  Controller d;
  d.apply(Cmd::TRUE_, 0);
  d.apply(Cmd::TRUE_, 1900);
  return !d.update(3899) && d.greenOn() && d.update(3900);
}
static_assert(restart(), "reinicio del temporizador y cambio de LED");

// ---- Mensaje inválido: no cambia nada ni reinicia ----
constexpr bool ignoreInvalid() {
  Controller c;
  c.apply(Cmd::TRUE_, 0);
  if (c.apply(Cmd::INVALID, 1500)) return false;      // ignorado
  if (!c.greenOn()) return false;                     // sigue verde
  if (!c.update(2000) || c.greenOn()) return false;   // el temporizador original NO se reinició
  Controller d;                                       // inválido con todo apagado: sigue apagado
  return !d.apply(Cmd::INVALID, 100) && !d.greenOn() && !d.redOn();
}
static_assert(ignoreInvalid(), "inválido se ignora sin reiniciar");

// ---- Desbordamiento de millis() (uint32) ----
constexpr bool wrap() {
  Controller c;
  c.apply(Cmd::FALSE_, 0xFFFFFC00u);          // 1024 ms antes del desbordamiento
  if (c.update(0xFFFFFFFFu) || !c.redOn()) return false;   // 1023 ms
  if (c.update(0x000003CFu) || !c.redOn()) return false;   // 1999 ms (cruzó 0)
  return c.update(0x000003D0u) && !c.redOn();              // 2000 ms
}
static_assert(wrap(), "correcto al desbordar millis()");

// ---- Invariante: nunca ambos encendidos en ninguna secuencia ----
constexpr bool neverBoth() {
  Controller c;
  const Cmd seq[6] = {Cmd::TRUE_, Cmd::FALSE_, Cmd::INVALID, Cmd::TRUE_, Cmd::FALSE_, Cmd::TRUE_};
  uint32_t t = 0;
  for (Cmd x : seq) {
    c.apply(x, t);
    if (c.greenOn() && c.redOn()) return false;
    t += 700;
    c.update(t);
    if (c.greenOn() && c.redOn()) return false;
  }
  return true;
}
static_assert(neverBoth(), "nunca ambos encendidos");

// ---- Pines pedidos ----
static_assert(GREEN_PIN == 18 && RED_PIN == 19 && ON_MS == 2000, "pines y duracion");

int main() { return 0; }
