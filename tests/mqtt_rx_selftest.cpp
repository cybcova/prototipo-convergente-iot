// Pruebas sin hardware de la clasificación de mensajes MQTT recibidos y de los tópicos.
// Se validan en compilación con static_assert (no hay compilador host en este entorno).
#include <initializer_list>
#include "../LilyGoReceptor/mqtt_config.h"
#include "../LilyGoReceptor/mqtt_rx.h"

using namespace mqttrx;

constexpr size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
constexpr bool eq(const char *a, const char *b) { size_t i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return false; return a[i] == b[i]; }
constexpr bool has(const char *s, char c) { for (size_t i = 0; s[i]; i++) if (s[i] == c) return true; return false; }

// Clasifica una cadena literal (con bytes arbitrarios dados como arreglo)
constexpr Kind K(std::initializer_list<int> v) {
  uint8_t b[64] = {};
  size_t n = 0;
  for (int x : v) b[n++] = static_cast<uint8_t>(x);
  return classify(b, n);
}
constexpr Kind T(const char *s) {  // texto C: sin el NUL terminal
  uint8_t b[600] = {};
  size_t n = slen(s);
  for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>(s[i]);
  return classify(b, n);
}

// ---- Tópicos ----
static_assert(eq(MQTT_TOPIC_RESPONSE, "iot/prototipo-convergente/response"), "topico de respuesta pedido");
static_assert(eq(MQTT_TOPIC_EVENTS, "iot/prototipo-convergente/rfid/events"), "topico RFID intacto");
static_assert(!eq(MQTT_TOPIC_RESPONSE, MQTT_TOPIC_EVENTS), "suscripcion != publicacion (no se reciben los propios eventos)");
static_assert(!has(MQTT_TOPIC_RESPONSE, '+') && !has(MQTT_TOPIC_RESPONSE, '#'), "suscripcion exacta, sin comodines");

// ---- Vacío / texto / JSON ----
static_assert(classify(nullptr, 0) == Kind::EMPTY, "nulo");
static_assert(classify(nullptr, 5) == Kind::EMPTY, "puntero nulo");
static_assert(T("") == Kind::EMPTY, "vacio");
static_assert(T("hola mundo") == Kind::TEXT, "texto plano");
static_assert(T("{\"ok\":true}") == Kind::JSON_LIKE, "objeto JSON");
static_assert(T("  \n[1,2,3]\r\n ") == Kind::JSON_LIKE, "arreglo JSON con espacios");
static_assert(T("{no cerrado") == Kind::TEXT, "no parece JSON si no cierra");
static_assert(T("{\"a\":\n1}\t") == Kind::JSON_LIKE, "saltos y tabs permitidos");
static_assert(T("   ") == Kind::TEXT, "solo espacios es texto");

// ---- UTF-8 válido ----
static_assert(K({'a', 0xC3, 0xB1, 'b'}) == Kind::TEXT, "n con tilde (2 bytes)");
static_assert(K({0xE2, 0x82, 0xAC}) == Kind::TEXT, "euro (3 bytes)");
static_assert(K({0xF0, 0x9F, 0x98, 0x80}) == Kind::TEXT, "emoji (4 bytes)");
static_assert(K({'{', '"', 'n', '"', ':', '"', 0xC3, 0xB1, '"', '}'}) == Kind::JSON_LIKE, "JSON con UTF-8");

// ---- No texto / UTF-8 inválido / controles ----
static_assert(K({0x00}) == Kind::BINARY, "NUL");
static_assert(K({'a', 0x00, 'b'}) == Kind::BINARY, "NUL embebido");
static_assert(K({0x07}) == Kind::BINARY, "campana");
static_assert(K({0x1B, '[', '3', '1', 'm'}) == Kind::BINARY, "ESC (secuencias de terminal)");
static_assert(K({0x7F}) == Kind::BINARY, "DEL");
static_assert(K({0xFF}) == Kind::BINARY, "0xFF");
static_assert(K({0xC3}) == Kind::BINARY, "secuencia incompleta al final");
static_assert(K({0xC3, 'a'}) == Kind::BINARY, "continuacion invalida");
static_assert(K({0x80}) == Kind::BINARY, "continuacion suelta");
static_assert(K({0xC0, 0x80}) == Kind::BINARY, "sobrelarga C0");
static_assert(K({0xE0, 0x80, 0x80}) == Kind::BINARY, "sobrelarga E0");
static_assert(K({0xED, 0xA0, 0x80}) == Kind::BINARY, "sustituto UTF-16");
static_assert(K({0xF4, 0x90, 0x80, 0x80}) == Kind::BINARY, "mayor que U+10FFFF");
static_assert(K({0xF5, 0x80, 0x80, 0x80}) == Kind::BINARY, "F5 invalido");
static_assert(K({0xE2, 0x82}) == Kind::BINARY, "3 bytes truncado");
static_assert(K({0x01, 0x02, 0x03, 0x04}) == Kind::BINARY, "binario puro");

// ---- Tamaño: nunca lee fuera del rango dado (secuencia multibyte cortada en el límite) ----
constexpr bool boundary() {
  const uint8_t s[4] = {'a', 0xE2, 0x82, 0xAC};   // "a€"
  return classify(s, 4) == Kind::TEXT && classify(s, 3) == Kind::BINARY && classify(s, 2) == Kind::BINARY && classify(s, 1) == Kind::TEXT;
}
static_assert(boundary(), "limites de longitud");

// ---- Un payload tan grande como lo que PubSubClient puede entregar con buffer de 512 B ----
constexpr bool bigText() {
  uint8_t b[512] = {};
  for (size_t i = 0; i < 470; i++) b[i] = 'x';
  return classify(b, 470) == Kind::TEXT;
}
static_assert(bigText(), "payload grande");

int main() { return 0; }
