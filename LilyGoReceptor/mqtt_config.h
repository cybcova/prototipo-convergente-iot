// Configuración celular/MQTT del LilyGoReceptor. Fuente de verdad: Ejemplos/lilygoTelcel/lilygoTelcel.ino
// (probado físicamente por el usuario). Los valores de operador y broker se COPIARON del ejemplo sin
// cambios; solo el tópico y el tamaño del buffer son propios de este proyecto (ver DISENO_MQTT_RFID.md).
//
// Seguridad: son valores genéricos del operador (APN) y un broker público SIN autenticación ni TLS, tal
// como en el ejemplo; no contienen secretos propios. Esta es la ÚNICA copia en este firmware y NUNCA se
// imprimen en logs. Si algún día se sustituyen por credenciales reales, mover este archivo fuera del
// control de versiones (.gitignore) y dejar una plantilla.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---------- Red celular (idéntico al ejemplo) ----------
constexpr char MQTT_APN[]       = "internet.itelcel.com";
constexpr char MQTT_GPRS_USER[] = "itelcel";
constexpr char MQTT_GPRS_PASS[] = "itelcel";

// ---------- Broker (idéntico al ejemplo: público, puerto 1883 sin TLS, sin usuario/clave) ----------
constexpr char MQTT_BROKER[] = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT = 1883;

// ---------- Tópico FIJO y EXCLUSIVO de este proyecto ----------
// No depende del Client ID ni del IMEI, no es aleatorio y NO reutiliza el del ejemplo
// ("GsmClientTest/<imei>/potenciometro"). Sin comodines (+ #): un tópico de publicación no puede tenerlos.
constexpr char MQTT_TOPIC_EVENTS[] = "iot/prototipo-convergente/rfid/events";

// ---------- Hardware del módem (idéntico al ejemplo, T-SIM7070G) ----------
constexpr int MODEM_PWR_PIN = 4;
constexpr int MODEM_UART_RX = 26;   // GPIO26 <- TX del módem
constexpr int MODEM_UART_TX = 27;   // GPIO27 -> RX del módem
constexpr unsigned long MODEM_BAUD = 115200;

// ---------- Tiempos del ejemplo ----------
constexpr unsigned long MODEM_POWER_PULSE_MS = 1000;    // PWR HIGH 1 s y luego LOW
constexpr unsigned long MODEM_BOOT_WAIT_MS = 6000;      // espera tras abrir la UART del módem
constexpr unsigned long NETWORK_WAIT_MS = 180000UL;     // waitForNetwork
constexpr unsigned long GPRS_RETRY_PAUSE_MS = 10000;    // pausa tras fallo de gprsConnect
constexpr unsigned long MQTT_RETRY_MS = 10000;          // intervalo entre intentos de mqttConnect

// ---------- Propio de este proyecto ----------
// PubSubClient 2.8 trae 256 B por defecto: el JSON de un evento no cabe y publish() devolvería false.
constexpr uint16_t MQTT_BUFFER_BYTES = 512;
constexpr size_t PENDING_QUEUE_LEN = 32;   // eventos en RAM pendientes de publicar (sin persistencia)
constexpr uint8_t PUBLISH_MAX_ATTEMPTS = 3; // con MQTT conectado, intentos antes de descartar un evento inservible
