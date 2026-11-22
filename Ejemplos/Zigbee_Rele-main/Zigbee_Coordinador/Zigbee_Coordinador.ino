/* =========================================================================
   CÓDIGO DIDÁCTICO: ZIGBEE SWITCH / COORDINADOR (COMENTADO A DETALLE)
   Materia: Internet de las Cosas
   Docente: MGTI. Saul Isai Soto Ortiz
   
   ¿CÓMO FUNCIONA ESTE NODO?
   Este dispositivo actuará como un "Zigbee Coordinator" (Coordinador).
   En toda red Zigbee DEBE existir exactamente un Coordinador. Es el "Jefe".
   Su trabajo es elegir el canal (frecuencia), crear la red (asignar el PAN ID),
   y abrir la "puerta" para permitir que otros dispositivos (Focos, Sensores) 
   se unan a su red. Una vez creada la red, también puede enviar comandos.
   ========================================================================= */

// --- PROTECCIÓN CONTRA ERRORES DE ALUMNOS ---
// Si un alumno olvida cambiar el modo en el menú de Arduino, el código no compilará.
// Esto evita horas de frustración tratando de averiguar por qué la placa no crea la red.
#ifndef ZIGBEE_MODE_ZCZR
#error "¡ERROR! Selecciona 'Zigbee ZCZR' en el menú Tools -> Zigbee mode"
#endif

#include "Zigbee.h"

// --- DEFINICIÓN DE HARDWARE (EL BOTÓN SEGURO) ---
// Usamos el GPIO 18 porque es un pin "limpio". Si usáramos los pines 8 o 9 
// (Strapping Pins) y el alumno los conecta a tierra al conectar el USB, 
// el ESP32 se asustaría y entraría en modo "waiting for download".
const uint8_t PIN_BOTON = 18; 

// --- CONCEPTOS ZIGBEE: LOS ENDPOINTS ---
// Al igual que el foco, el apagador también tiene su "número de departamento".
// Esto permite tener un ESP32 con 4 botones físicos controlando 4 focos distintos.
const uint8_t ENDPOINT_SWITCH = 1; 

// Creamos el objeto virtual "Interruptor".
// Usamos ZigbeeColorDimmerSwitch (en vez de ZigbeeSwitch) porque, además de Toggle,
// puede enviar el comando "Level" (nivel 0-255). Ese nivel es el que usamos para
// transportar el contador del latido por el aire hacia el Router.
ZigbeeColorDimmerSwitch zbSwitch = ZigbeeColorDimmerSwitch(ENDPOINT_SWITCH);


// --- VARIABLES DE INGENIERÍA: FILTRO ANTIRREBOTE (DEBOUNCE) ---
// Cuando dos metales chocan al presionar un botón físico, a nivel microscópico
// rebotan varias veces antes de estabilizarse. Si no usamos esto, el ESP32 
// pensaría que presionamos el botón 10 veces en un milisegundo.

bool estadoAnteriorBoton = HIGH; 
unsigned long ultimoTiempoRebote = 0;
unsigned long retardoRebote = 50; // Ignoraremos cualquier ruido que dure menos de 50 milisegundos

// --- CHECADOR DE COMUNICACIÓN (HEARTBEAT TRANSMITIDO) ---
// Cada INTERVALO_LATIDO el Coordinador ENVÍA por Zigbee un número consecutivo al
// Router. El Router imprime el mismo número al recibirlo, así que si ves el mismo
// "#N" en los dos monitores seriales, la comunicación está viva.
// Si los números dejan de coincidir, se perdió la comunicación (o hay latidos perdidos).
//
// El contador viaja en el atributo "nivel" (0-255) del cluster Level Control.
// Va de 1 a 254 y luego regresa a 1 (el 0 y el 255 se evitan: 0 puede interpretarse
// como "apagado" y 255 es el valor inicial por defecto del Router).
// Usamos millis() en lugar de delay() para no bloquear la lectura del botón.
const char* ROL_NODO = "COORDINADOR";
const unsigned long INTERVALO_LATIDO = 5000; // ms entre cada latido
unsigned long ultimoLatido = 0;
uint8_t contadorLatidos = 0;

void enviarLatido() {
  if (millis() - ultimoLatido < INTERVALO_LATIDO) return;
  ultimoLatido = millis();

  // Sin binding no hay a quién enviarle el latido (el Router aún no se vincula).
  if (!zbSwitch.bound()) {
    Serial.printf("[%s] Latido NO enviado: aún no hay Router vinculado (binding)\n", ROL_NODO);
    return;
  }

  contadorLatidos = (contadorLatidos >= 254) ? 1 : contadorLatidos + 1;
  zbSwitch.setLightLevel(contadorLatidos);
  Serial.printf("[%s] ♥ Latido #%u ENVIADO por Zigbee -> Router | uptime: %lu s\n",
                ROL_NODO, contadorLatidos, millis() / 1000);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- NODO SWITCH (COORDINADOR) INICIANDO ---");
  
  // --- CONCEPTOS DE ELECTRÓNICA: INPUT_PULLUP ---
  // Activamos una resistencia interna del chip conectada a 3.3V.
  // Esto mantiene el pin 18 en estado HIGH (1 lógico) constantemente.
  // Cuando el alumno conecta el pin 18 a GND (Tierra) con un cable, 
  // la corriente se "cae", el pin lee un LOW (0 lógico) y detectamos el "clic".
  
  pinMode(PIN_BOTON, INPUT_PULLUP);

  // Registramos nuestro dispositivo con un nombre personalizado en la red
  zbSwitch.setManufacturerAndModel("ClaseIng_UAEH", "ApagadorMaestro_V1");
  Zigbee.addEndpoint(&zbSwitch);

  // Abrimos la red (Permit Join) 180 s después de cada arranque. Lo pedimos de forma
  // explícita porque el binding solo se crea mientras la red está abierta y un
  // dispositivo se está uniendo; así no dependemos del valor por defecto.
  Zigbee.setRebootOpenNetwork(180);

  // --- CONCEPTOS ZIGBEE: AISLAMIENTO DE CANAL ---
  // Obligamos al Coordinador a fundar su red exclusivamente en esta frecuencia.
  // Si no hacemos esto, buscará el canal con menos ruido y podría cambiar 
  // cada vez que lo reiniciamos, perdiendo a los focos que ya tenía registrados.
  uint8_t canal_asignado = 11; // Modificar por equipo para no pelear en el laboratorio
  Zigbee.setPrimaryChannelMask(1 << canal_asignado);

  Serial.printf("[SISTEMA] Creando red aislada en el Canal %d...\n", canal_asignado);
  
  // --- EL MOMENTO DE LA CREACIÓN ---
  // Aquí le decimos al radio: "Eres el Jefe, inicia la red".
  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) { 
    Serial.println("[ERROR] Zigbee no inició. Revisa particiones en el menú Tools.");
    while (1); // Nos quedamos atrapados aquí si hay error (bucle infinito de seguridad)
  }

  // Al arrancar exitosamente como Coordinador, el ESP32 automáticamente 
  // abre una ventana de emparejamiento (Permit Join) por 3 minutos (180 segundos).
  // Durante este tiempo, cualquier foco nuevo que busque red podrá unirse.
  Serial.println("[SISTEMA] Red Zigbee lista. Ventana de emparejamiento ABIERTA.");
}

void loop() {
  enviarLatido(); // Envía el latido al Router cada INTERVALO_LATIDO

  // Leemos el voltaje actual del pin 18 (HIGH = Suelto, LOW = Presionado a Tierra)
  bool lecturaBoton = digitalRead(PIN_BOTON);

  // --- LÓGICA DEL ANTIRREBOTE ---
  // Si el estado cambió (alguien tocó el cable o hubo ruido electromagnético)
  if (lecturaBoton != estadoAnteriorBoton) {
    ultimoTiempoRebote = millis(); // Reiniciamos el cronómetro
  }

  // Si el estado se ha mantenido estable por más de 50ms (es un toque real)
  if ((millis() - ultimoTiempoRebote) > retardoRebote) {
    
    // Si la lectura estable es LOW (0 voltios), significa que el botón está presionado
    if (lecturaBoton == LOW) {
      Serial.println("\n[ACCIÓN] ¡Botón presionado!");
      Serial.println("[ZIGBEE] Enviando comando 'TOGGLE' a la red...");
      
      // --- CONCEPTOS ZIGBEE: COMANDO TOGGLE ---
      // El Coordinador no manda un "Enciende" o un "Apaga". Manda un comando 
      // universal "Toggle" (Alternar). Si el foco estaba prendido, se apaga; 
      // si estaba apagado, se prende. La red se encarga de enviarlo a los focos vinculados.
      zbSwitch.lightToggle(); 
      
      delay(500); // Pequeña pausa para no saturar el espectro de radio si el alumno deja el cable pegado
    }
  }
  
  // Guardamos el estado actual para compararlo en la siguiente vuelta del loop
  estadoAnteriorBoton = lecturaBoton;
}