/* =========================================================================
   CÓDIGO DIDÁCTICO: ZIGBEE LUZ / ACTUADOR (COMENTADO A DETALLE)
   Materia: Internet de las Cosas
   Docente: MGTI. Saul Isai Soto Ortiz
   
   ¿CÓMO FUNCIONA ESTE NODO?
   Este dispositivo actuará como un "Zigbee Router" (Enrutador). 
   A diferencia del Wi-Fi donde todos se conectan al módem, en Zigbee los 
   Routers siempre están despiertos, escuchan comandos y, además, ayudan a 
   rebotar la señal hacia otros dispositivos más lejanos (Red Mesh o Malla).
   ========================================================================= */

// --- PROTECCIÓN CONTRA ERRORES DE ALUMNOS ---
// Si un alumno elige "Zigbee ED" (End Device) en el menú, el ESP32 pensará 
// que funciona con baterías y apagará su antena para ahorrar energía (Sleepy Node).
// ¡Un foco no puede estar dormido, debe estar siempre listo para encenderse!
#ifdef ZIGBEE_MODE_ED
#error "¡ERROR! Para un actuador usa 'Zigbee Router', NO uses 'Zigbee ED'."
#endif

#include "Zigbee.h"

// --- DEFINICIÓN DE HARDWARE ---
// Usamos el pin 4 porque es seguro. (Los pines 8 y 9 son "Strapping Pins" 
// y pueden bloquear el arranque del ESP32 o dar un voltaje muy débil).
const uint8_t PIN_RELEVADOR = 4; 

// Buscamos automáticamente el pin del LED azul integrado en la placa
#ifndef LED_BUILTIN
#define LED_BUILTIN 15 
#endif
const uint8_t PIN_LED_INTERNO = LED_BUILTIN; 

// --- CONCEPTOS ZIGBEE: LOS ENDPOINTS (PUNTOS FINALES) ---
// Imagina que el ESP32 es un edificio de departamentos (El Nodo) y el 
// ENDPOINT es el número de un departamento específico.
// Si tuviéramos un ESP32 controlando 3 focos distintos, cada foco tendría 
// su propio número de departamento (Endpoint 10, Endpoint 11, Endpoint 12).

const uint8_t ENDPOINT_LUZ = 10; 

// Creamos el objeto virtual que representará nuestra luz en la red Zigbee,
// asignándole su "número de departamento".
// Usamos ZigbeeDimmableLight (en vez de ZigbeeLight) porque además de On/Off
// recibe el "nivel" (0-255), que es el canal por donde llega el latido del Coordinador.
ZigbeeDimmableLight zbLuz = ZigbeeDimmableLight(ENDPOINT_LUZ);


// --- RECEPTOR DEL LATIDO (HEARTBEAT TRANSMITIDO) ---
// El Coordinador manda un número consecutivo (1-254) en el "nivel" de la luz.
// Aquí lo imprimimos con el mismo "#N" que muestra el Coordinador al enviarlo,
// para comparar ambos monitores seriales. Los latidos perdidos se detectan por saltos
// en la numeración (ej. llega #5 y luego #7 = se perdió el #6).
const char* ROL_NODO = "ROUTER";
uint8_t ultimoNivelRecibido = 255;   // 255 = valor inicial por defecto, aún sin latidos
unsigned long latidosRecibidos = 0;
unsigned long ultimoLatidoRecibido = 0; // millis() de la última recepción
const unsigned long TIMEOUT_LATIDO = 15000; // ms sin latidos = comunicación perdida

void registrarLatido(uint8_t nivel) {
  if (ultimoNivelRecibido != 255) {
    uint8_t esperado = (ultimoNivelRecibido >= 254) ? 1 : ultimoNivelRecibido + 1;
    if (nivel != esperado) {
      Serial.printf("[%s] ⚠ Salto en latidos: esperaba #%u y llegó #%u (se perdieron latidos)\n",
                    ROL_NODO, esperado, nivel);
    }
  }
  ultimoNivelRecibido = nivel;
  latidosRecibidos++;
  ultimoLatidoRecibido = millis();
  Serial.printf("[%s] ♥ Latido #%u RECIBIDO por Zigbee <- Coordinador | total recibidos: %lu\n",
                ROL_NODO, nivel, latidosRecibidos);
}

void checarComunicacion() {
  // Avisamos una sola vez cuando se pierde la comunicación, y otra al recuperarse
  static bool avisoPerdida = false;
  if (latidosRecibidos == 0) return; // aún no ha llegado el primer latido
  bool perdida = (millis() - ultimoLatidoRecibido) > TIMEOUT_LATIDO;
  if (perdida && !avisoPerdida) {
    Serial.printf("[%s] ✖ Sin latidos del Coordinador hace más de %lu s: COMUNICACIÓN PERDIDA\n",
                  ROL_NODO, TIMEOUT_LATIDO / 1000);
  } else if (!perdida && avisoPerdida) {
    Serial.printf("[%s] ✔ Comunicación con el Coordinador RECUPERADA\n", ROL_NODO);
  }
  avisoPerdida = perdida;
}

// --- FUNCIÓN CALLBACK (INTERRUPCIÓN ASÍNCRONA) ---
// El ESP32 no está "preguntando" a cada rato si presionaros el botón (eso gastaría
// mucha memoria). En su lugar, el radio Zigbee trabaja en segundo plano.
// Cuando recibe mágicamente una orden por el aire, el radio "interrumpe" al ESP32
// y lo obliga a ejecutar esta función inmediatamente.
//
// Se dispara cuando cambia el estado (on/off) O el nivel:
//  - Si el nivel es distinto al último recibido, es un latido del Coordinador.
//  - Si cambió el estado, es una orden de encender/apagar el foco.
// El relevador siempre sigue al estado, así que el latido no lo afecta.
void accionarRelevador(bool estado, uint8_t nivel) {
  if (nivel != ultimoNivelRecibido) {
    registrarLatido(nivel);
  }

  static bool estadoAnterior = false;
  if (estado != estadoAnterior) {
    estadoAnterior = estado;
    Serial.printf("[%s] Comando Zigbee recibido: %s\n", ROL_NODO, estado ? "ENCENDER" : "APAGAR");
  }

  digitalWrite(PIN_RELEVADOR, estado ? HIGH : LOW);     // HIGH deja pasar los 3.3V al módulo Grove
  digitalWrite(PIN_LED_INTERNO, estado ? HIGH : LOW);   // LED de prueba
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- NODO ACTUADOR (LUZ) INICIANDO ---");

  // Configuramos los pines físicos
  pinMode(PIN_RELEVADOR, OUTPUT);
  pinMode(PIN_LED_INTERNO, OUTPUT);
  digitalWrite(PIN_RELEVADOR, LOW); 
  digitalWrite(PIN_LED_INTERNO, LOW);

  // --- CONCEPTOS ZIGBEE: CLUSTERS Y ATRIBUTOS ---
  // Zigbee es un estándar universal. Le decimos a la red exactamente quiénes somos 
  // para que incluso un dispositivo de marca (como Philips Hue o Alexa) nos reconozca.
  zbLuz.setManufacturerAndModel("ClaseIng_UAEH", "FocoRelevador_V1");
  
  // Le decimos al objeto virtual qué función debe ejecutar cuando reciba un mensaje
  zbLuz.onLightChange(accionarRelevador);
  
  // Registramos nuestro "departamento" en el directorio del "edificio" (ESP32)
  Zigbee.addEndpoint(&zbLuz);

  // --- CONCEPTOS ZIGBEE: AISLAMIENTO DE CANAL (LA SINTONÍA) ---
  // Zigbee comparte la frecuencia de 2.4GHz con el Wi-Fi, dividida en 16 canales (11 al 26).
  // Para evitar que los alumnos controlen los focos de otros equipos por error, 
  // forzamos al ESP32 a escuchar SOLO una "estación de radio" específica.
  uint8_t canal_asignado = 11; // Cambiar por el número de equipo
  
  // Usamos una máscara de bits (1 << 11). Es como un panel de 32 interruptores 
  // donde solo encendemos el interruptor número 11.
  Zigbee.setPrimaryChannelMask(1 << canal_asignado);

  Serial.printf("[SISTEMA] Sintonizando el radio en el Canal %d...\n", canal_asignado);

  // Arrancamos el hardware de radiofrecuencia (La antena)
  if (!Zigbee.begin()) {
    Serial.println("[ERROR] El radio Zigbee no pudo iniciar (Falta partición Zigbee 4MB).");
    while(1); 
  }

  // Zigbee.begin() solo indica que el radio arrancó, NO que ya nos unimos a la red.
  // Esperamos a estar realmente conectados al Coordinador antes de seguir.
  Serial.print("[SISTEMA] Buscando al Coordinador en el canal");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("[SISTEMA] Conectado a la red Zigbee correctamente.");
  
  // --- CONCEPTOS ZIGBEE: LA TABLA DE VINCULACIÓN (BINDING TABLE) ---
  // Aunque ya estamos en la red, el Foco y el Apagador no saben quién es quién.
  // Durante estos 30 segundos, intercambian sus direcciones de red (como pasarse 
  // el número de teléfono) y las guardan en su memoria flash para no olvidarse.
  Serial.println("[SISTEMA] Esperando creación de Binding Table (30 segundos)...");
}

void loop() {
  checarComunicacion(); // Avisa si dejan de llegar latidos del Coordinador

  // En Arduino normal, un delay(100) detendría todo. 
  // En el ESP32 (que usa el sistema operativo FreeRTOS), este delay solo pone 
  // a dormir la tarea principal, permitiendo que la tarea de la antena Zigbee 
  // procese los mensajes entrantes en segundo plano sin interrupciones.
  delay(100); 
}