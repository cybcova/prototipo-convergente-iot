# Auditoría de `Ejemplos/lilygoTelcel` (fuente de verdad de la conexión celular + MQTT)

Alcance: lectura del único archivo del ejemplo (`lilygoTelcel.ino`, 195 líneas; no hay README, esquemas ni archivos de configuración). No se modificó. Se verificó únicamente que **compila** tal cual con las librerías instaladas (ESP32 Dev Module, core 3.3.8: 312 592 B). **No se ejecutó**: el comportamiento en ejecución descrito aquí se deduce del código y de lo que el usuario informa (probado físicamente).

Por seguridad, **no se reproducen aquí el usuario ni la clave del APN** (siguen en el ejemplo y en `LilyGoReceptor/mqtt_config.h`).

## 1. Qué hace el ejemplo
Lee un potenciómetro (ADC GPIO34) y publica un JSON por MQTT sobre red celular LTE-M/NB-IoT/2G, desde una LilyGO T-SIM7070G. La parte de potenciómetro no es relevante para RFID; sí lo es todo lo demás.

## 2. Hallazgos solicitados
| Elemento | Valor / comportamiento | Evidencia |
|---|---|---|
| Placa | **LilyGO T-SIM7070G** (comentario del encabezado). Sin opciones de placa de Arduino IDE documentadas. Se compiló con `ESP32 Dev Module`. | línea 3 |
| Librerías | **TinyGSM** (`#define TINY_GSM_MODEM_SIM7070`, `TinyGsmClient.h`) y **PubSubClient**. Versiones no registradas en el ejemplo; instaladas localmente: TinyGSM **0.12.0**, PubSubClient **2.8**. | líneas 5, 10–11; `arduino-cli lib list` |
| Módem | `TinyGsm modem(SerialAT)`; `SerialAT = Serial1`. UART del módem: **115200 8N1, RX=GPIO26, TX=GPIO27**. | líneas 8, 31, 140 |
| Encendido | `PWR_PIN` = **GPIO4**: HIGH 1000 ms y luego LOW; después `SerialAT.begin(...)`, `delay(6000)`, `modem.restart()`. | líneas 22, 134–143 |
| Identidad | `imei = modem.getIMEI()`. | línea 144 |
| Modo de red | `modem.setNetworkMode(2)` (automático) y `modem.setPreferredMode(3)` (LTE-M > NB-IoT > GSM). | líneas 150–151 |
| Operador/APN | APN `internet.itelcel.com` (Telcel), con usuario y clave de APN (valores genéricos del operador; no se copian aquí). | líneas 14–16 |
| Conexión celular | En `loop()`: si `!isNetworkConnected()` → `waitForNetwork(180000L, true)` (si falla, vuelve a intentar en el siguiente `loop()`) y `identificarTecnologiaRed()` (`AT+CPSI?`). | líneas 164–168 |
| Datos (IP) | si `!isGprsConnected()` → `gprsConnect(apn, user, pass)`; si falla: mensaje, **`delay(10000)`** y reintento. | líneas 171–180 |
| Broker | **`broker.hivemq.com`, puerto 1883**, MQTT sin cifrado (**sin TLS**), **sin usuario ni contraseña** (`mqtt.connect(imei)` solo con Client ID). Broker **público**. | líneas 18, 72, 153 |
| Autenticación / TLS | Ninguna. No hay certificados. `TinyGsmClient` (no `...Secure`). | código |
| Client ID / sesión | Client ID = **IMEI**. Sin Last-Will, sin parámetros de sesión: PubSubClient por defecto (clean session, keepalive 15 s, timeout de socket 15 s, buffer 256 B). | línea 72; cabecera PubSubClient 2.8 |
| Tópico | `GsmClientTest/<IMEI>/potenciometro` (armado en `setup()`). | línea 147 |
| Reintentos MQTT | si `!mqtt.connected()`: un intento cada **10 s** (`lastReconnectAttempt`), luego `delay(100)` y `return`. | líneas 183–191 |
| Publicación | `mqtt.publish(topic, json, true)`: **QoS 0** (PubSubClient solo publica QoS 0) y **retenido**. `publish()` devuelve `true` al escribir en el socket; no hay confirmación del broker. Si devuelve `false`, el valor no se marca como enviado y se reintenta en la siguiente evaluación. | línea 115 |
| Mantenimiento | `mqtt.loop()` en cada vuelta mientras haya conexión (keepalive). | línea 193 |
| Formato de mensaje | JSON armado a mano con `String`: `{"dispositivo":"<imei>","pot_raw":…,"pot_porcentaje":…,"voltaje":…,"red":"<tecnología>"}`. | líneas 107–113 |
| Pérdida de conexión | Se detecta en `loop()` (red → datos → MQTT, en ese orden) y se recupera repitiendo la misma secuencia. No reinicia el módem ni hay watchdog propio. | `loop()` |
| Tiempos clave | 1 s (pulso PWR), 6 s (arranque del módem), 180 s (espera de red), 10 s (pausa tras fallo de APN), 10 s (entre intentos MQTT), 100 ms. | — |

## 3. Observaciones y riesgos del ejemplo (a tener en cuenta al reutilizarlo)
1. **Broker público sin autenticación ni TLS**: cualquiera puede suscribirse al tópico o publicar en él; el contenido viaja en claro.
2. **IMEI en el tópico y en el payload** (identificador del dispositivo expuesto públicamente).
3. **Publicación retenida** (`retain=true`): adecuada para "último valor", no para un flujo de eventos (un suscriptor nuevo recibiría el último evento como si fuera actual).
4. **Buffer MQTT por defecto de 256 B**: un JSON mayor haría que `publish()` devuelva `false` sin más explicación. El del ejemplo cabe; el de RFID (≈ 300 B) no.
5. **Todo el flujo es bloqueante en `loop()`** (`waitForNetwork` hasta 180 s, `delay(10000)`): válido para un potenciómetro, **inaceptable** para un receptor UART que no debe dejar de leer.
6. La pulsación de `PWR_PIN` en cada arranque *alterna* el módem en esta familia de módems; si el módem ya estuviera encendido (reinicio blando del ESP32), podría apagarse. **No verificado**; el ejemplo lo hace igual y se replicó.
7. El ejemplo registra el **IMEI** por el monitor; es un identificador local, no una credencial.
8. Sin LWT, sin QoS > 0, sin reintento con *backoff*: la fiabilidad es "mejor esfuerzo".

## 4. Lo que NO se puede afirmar
- Versiones exactas de librerías con las que se probó (solo las instaladas hoy).
- Cualquier medida en vivo (latencias, comportamiento del módem, reconexión real).
- Si `broker.hivemq.com` seguirá aceptando conexiones sin credenciales.
- La placa exacta de Arduino IDE usada por el usuario.
