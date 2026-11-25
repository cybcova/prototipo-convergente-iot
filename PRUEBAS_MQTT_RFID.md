# Pruebas — publicación MQTT de eventos RFID (LilyGO 7070)

Fecha: 2026-11-25. Diseño: `DISENO_MQTT_RFID.md`. Referencia: `AUDITORIA_LILYGO_TELCEL.md`.
**Ninguna prueba con red celular, módem ni broker se ha ejecutado.** Todo lo de esta lista "PASA" es compilación o pruebas de formato locales.

## 1. Ejecutado (local)
Comando: `bash tests/run_checks.sh`

| Categoría | Prueba | Resultado |
|---|---|---|
| Compilación | `LilyGoReceptor` con TinyGSM 0.12.0 + PubSubClient 2.8 (`esp32:esp32:esp32`, core 3.3.8), 0 advertencias | PASA — 310 444 B (23 %), 25 764 B RAM estática |
| Compilación | `Ejemplos/lilygoTelcel` **sin modificar** (comprobación de que las librerías instaladas sirven para la referencia) | PASA — 312 592 B |
| Compilación | `EmisorC6` y `CoordinadorC6` (sin cambios en esta etapa) | PASA — 692 858 B y 694 628 B |
| Formato | Tópico: igual a `iot/prototipo-convergente/rfid/events`; **no** empieza con `GsmClientTest`; **no** contiene `potenciometro`; sin `+`, `#`, espacios ni `/` inicial | PASA (`static_assert`) |
| Formato | JSON exacto para evento REAL y para SIMULADO con IEEE desconocida (`null`) | PASA |
| Formato | UART → `Event` → JSON conserva todos los campos (línea de `trace_link.h` y JSON del mismo evento coinciden) | PASA |
| Formato | Peor caso (EPC de 32 B, valores máximos) = 366 B; con cabecera MQTT y tópico = 413 B ≤ 512 B; con el buffer por defecto de 256 B **no cabría** (justifica `setBufferSize`) | PASA |
| Formato | Buffer pequeño o nulo rechazado; sin caracteres que requieran escape; comillas balanceadas | PASA |
| Formato | El JSON esperado es JSON estricto y tiene 13 campos (verificado con el parser JSON de Python, fuera del repo) | PASA |
| Revisión | Los pines/APN/broker/puerto/Client ID/tiempos coinciden con el ejemplo (`static_assert` de pines y puerto + revisión de código) | PASA |
| Revisión | `ACK` se envía **antes** y sin depender de MQTT: `handleLine` → `showEvent` → `pendPush` (no bloquea) → `sendAck` | PASA (código) |
| Revisión | `loop()` nunca llama a `modem` ni a `mqtt`; `mqttTask` es el único que las usa | PASA (código) |
| Revisión | Cola: anillo con mutex; el productor descarta el más antiguo registrando seq/MSG ID; el consumidor retira la cabeza solo si sigue siendo la misma | PASA (código) |
| Revisión | Las credenciales del APN no se imprimen ni aparecen en ningún `.md` | PASA |
| Archivos intocados | `Ejemplos/lilygoTelcel`, `EmisorC6`, `CoordinadorC6` sin cambios (`git status`) | PASA |

Hallazgo de revisión que **cambió** el diseño: el core 3.3.8 tiene activo el watchdog de la tarea idle del core 0 (5 s, panic) y TinyGSM espera con `delay(0)`; por eso `mqttTask` está fijada al core 1.

## 2. Correspondencia con la validación solicitada
| # | Validación pedida | Estado |
|---|---|---|
| 1 | El receptor UART sigue funcionando | Compila y el código UART no cambió (solo se añadió el encolado); **física: pendiente** |
| 2 | Los eventos simulados llegan desde el coordinador | **Pendiente física** (ya comprobado en la etapa anterior) |
| 3 | El LilyGO publica en el nuevo tópico | **Pendiente física** (tópico verificado en código/tests) |
| 4 | El JSON contiene los datos correctos | **PASA en formato** (static_assert); contenido real **pendiente física** |
| 5 | El tópico es distinto al del ejemplo | **PASA** |
| 6 | Una desconexión MQTT no bloquea la UART | Por diseño (tarea separada); **pendiente física** |
| 7 | La reconexión sigue el mecanismo del ejemplo | Revisión de código **PASA**; física pendiente |
| 8 | Los pendientes se gestionan sin conexión | Lógica revisada; **sin prueba de ejecución** (no existe prueba host de la cola, vive en el `.ino`) |
| 9 | ACK UART y MQTT con semánticas independientes | **PASA** (código y documentación) |

## 3. Pendiente (requiere el LilyGO, SIM Telcel con datos y el broker)
Procedimiento recomendado, de menor a mayor riesgo:
1. **Referencia**: cargar `Ejemplos/lilygoTelcel` sin cambios y confirmar que publica (el usuario ya lo hizo). Anotar tecnología de red, IMEI y tiempo hasta conectar.
2. Cargar `LilyGoReceptor` con el cable C6→LilyGO desconectado: debe verse `[MQTT] Topico fijo…`, la secuencia del módem y la conexión, **sin** reinicios (vigilar el watchdog) y con `[ESTADISTICAS MQTT] estado=MQTT conectado`.
3. En otro equipo, suscribirse (cualquier cliente MQTT, p. ej. `mosquitto_sub -h broker.hivemq.com -t iot/prototipo-convergente/rfid/events -v`). Dado que el broker es **público**, el suscriptor puede ver también mensajes ajenos si alguien más usa ese tópico: filtrar por `coordinatorSession`/`senderIeee` propios.
4. Conectar el C6 coordinador (emisor en simulación cada 8 s): verificar en el LilyGO `[EVENTO RFID]` → `[MQTT] Evento encolado` → `[MQTT] Publicado seq N`, y en el suscriptor un JSON con **los mismos** `msgId`, `epc`, `origin:"SIMULADO"`, `seq`.
5. Comprobar `senderIeee` (¿IEEE real del emisor o `null`?).
6. **Pérdida de MQTT**: apagar/retirar la antena o la SIM unos minutos con eventos fluyendo: la consola del LilyGO debe seguir mostrando eventos y ACK (en el coordinador `ack` debe seguir creciendo), estado `sin red celular`/`sin MQTT`, `pendientes` creciendo. Al volver, los pendientes deben publicarse en orden.
7. **Cola llena**: mantenerlo sin conexión > 32 eventos (≈ 4 min): debe aparecer `descartado el mas antiguo (seq …)` y los `seq` faltantes deben corresponder al suscriptor.
8. **Estabilidad ≥ 30 min** sin reinicios ni `[ERROR RX]` nuevos.
9. Reinicio del LilyGO con pendientes: confirmar que se pierden (comportamiento documentado).
10. Registrar logs de las tres consolas (emisor, coordinador, LilyGO) y del suscriptor en este archivo.

## 4. Resultados en hardware
_Pendiente de ejecutar._

---

# Recepción MQTT — tópico `iot/prototipo-convergente/response`

Diseño: `DISENO_MQTT_RFID.md` §10. **No probada con el broker real ni con el módem.**

## Ejecutado (local, `bash tests/run_checks.sh`)
| Categoría | Prueba | Resultado |
|---|---|---|
| Compilación | `LilyGoReceptor` con la recepción (312 496 B, 25 788 B RAM estática), 0 advertencias | PASA |
| Formato | Tópico de respuesta exacto; publicación RFID intacta; **suscripción ≠ publicación** y sin comodines | PASA (`static_assert`) |
| Formato | Clasificación: vacío, texto, JSON (con espacios/saltos/UTF-8), no-JSON, UTF-8 válido de 2/3/4 bytes | PASA |
| Formato | No texto: NUL (también embebido), campana, ESC, DEL, 0xFF, secuencias incompletas o cortadas en el límite, sobrelargas (C0, E0), sustitutos, > U+10FFFF, binario puro | PASA |
| Formato | Nunca lee fuera de la longitud dada (multibyte cortado en el límite); payload de 470 B | PASA |
| Las pruebas pueden fallar | Mutación temporal del validador UTF-8 (fuera del repo) | Falla como se espera |
| Revisión de código | La suscripción ocurre **después** de `mqttConnect()` exitoso y en **cada** reconexión (`mqttSubscribed = false` mientras `!mqtt.connected()` + `subscribeResponse()` tras conectar); reintento si `subscribe()` falla | PASA |
| Revisión de código | El callback solo imprime y cuenta; no llama a `pendPush`, `publish` ni toca la cola; corre dentro de `mqtt.loop()` en `mqttTask`, no en `loop()` | PASA |
| Archivos intocados | `Ejemplos/lilygoTelcel`, `EmisorC6`, `CoordinadorC6` | PASA (`git status`) |

## Pendiente (requiere LilyGO + SIM con datos + broker)
1. Cargar `LilyGoReceptor`. Esperar `[MQTT] Conectando a MQTT... exito!` seguido de `[MQTT] Suscrito a iot/prototipo-convergente/response` (la suscripción debe aparecer **después** de conectar). `[ESTADISTICAS MQTT RX]` debe decir `suscrito=si`.
2. Desde otro equipo, publicar (sin `-r`; el broker es público):
   - Texto JSON: `mosquitto_pub -h broker.hivemq.com -p 1883 -t iot/prototipo-convergente/response -m '{"ok":true}'`
   - Texto plano: `… -m 'hola'`
   - UTF-8: `… -m 'canción ñ'`
   - Vacío: `… -n`
   - Binario: `… -f archivo.bin` (o `-m "$(printf '\001\377')"`)
   - Grande (> 470 B): debe **no** aparecer nada (descartado por PubSubClient).
   También sirve el cliente web de HiveMQ (`broker.hivemq.com`, puerto 8884/WebSocket).
3. Esperado en el monitor, por mensaje: `[MQTT RX] Topico: …`, `Longitud: N bytes`, `Tipo: …` y `Contenido: …` (hex si es binario).
4. Con eventos RFID/simulados fluyendo a la vez: los `[EVENTO RFID]`, ACK y `[MQTT] Publicado seq …` deben seguir **sin interrupciones** al recibir mensajes.
5. Comprobar que el mensaje recibido **no** aparece en `…/rfid/events` (suscribirse a ambos tópicos).
6. **Reconexión**: cortar la red (antena/SIM) y restablecerla; tras `[MQTT] … exito!` debe aparecer de nuevo `Suscrito a …` y un mensaje nuevo debe recibirse. Contar `suscripciones ok` en las estadísticas.
7. **Retenidos** (opcional): publicar con `-r`, reiniciar la conexión y observar que reaparece en cada suscripción; **limpiar** con `mosquitto_pub … -t iot/prototipo-convergente/response -n -r`.
8. Registrar los logs en este archivo.

## Resultados en hardware (recepción)
_Pendiente de ejecutar._

---

# LEDs por MQTT (GPIO18 verde / GPIO19 rojo)

Diseño: `DISENO_MQTT_RFID.md` §11. **No probada con LEDs ni broker.**

## Ejecutado (local, `bash tests/run_checks.sh`)
| Categoría | Prueba | Resultado |
|---|---|---|
| Compilación | `LilyGoReceptor` con LEDs (313 336 B, 25 804 B RAM estática), 0 advertencias | PASA |
| Lógica | Parseo exacto: `true`/`false` válidos; `TRUE`, `True`, prefijos, un carácter extra, con `\n` o espacio, `1/0/on`, JSON, vacío y puntero nulo → inválidos | PASA (`static_assert`) |
| Lógica | La longitud manda y no el NUL (basura fuera de la longitud no cuenta; NUL embebido invalida) | PASA |
| Lógica | Arranque apagado; `true`→verde, `false`→rojo; exclusivos | PASA |
| Lógica | Se apaga a **2000 ms exactos** (1999 ms sigue encendido); ya apagado no se vuelve a apagar | PASA |
| Lógica | Nuevo mensaje válido (también el mismo valor) cambia de LED y **reinicia** los 2 s | PASA |
| Lógica | Mensaje inválido **no** cambia el estado ni reinicia el temporizador | PASA |
| Lógica | Correcto al desbordar `millis()` (cruce de 2³²) | PASA |
| Lógica | Invariante: nunca ambos encendidos en una secuencia mezclada | PASA |
| Las pruebas pueden fallar | Mutación temporal (`>= 2000` → `> 2000`), fuera del repo | Falla (4 aserciones) |
| Revisión de código | El callback solo valida y encola; `ledPoll()` (en `loop()`) no usa `delay`, solo `millis()` y `xQueueReceive(…, 0)`; no toca el módem/MQTT/UART | PASA |
| Revisión de código | Pines a OUTPUT y LOW al principio de `setup()`; al cambiar se apaga antes de encender | PASA |
| Archivos intocados | `Ejemplos`, `EmisorC6`, `CoordinadorC6`; tópicos y credenciales sin cambios | PASA |

## Pendiente (requiere LilyGO, LEDs y broker)
1. Al encender/reiniciar la placa: ambos LEDs **apagados** (también durante el arranque del módem).
2. `mosquitto_pub -h broker.hivemq.com -p 1883 -t iot/prototipo-convergente/response -m true` → verde 2 s, rojo apagado, luego ambos apagados; el monitor muestra `[LED] Valor recibido: true -> LED VERDE (GPIO18)…` y `2000 ms cumplidos`.
3. Igual con `false` → rojo 2 s.
4. **Reinicio**: enviar `true`, y a los ~1 s enviar `false`: el verde debe apagarse, el rojo encenderse y permanecer **2 s completos desde el segundo mensaje**. Repetir con el mismo valor dos veces.
5. **Ignorados**: durante un LED encendido enviar `TRUE`, `hola`, `{"v":true}`, vacío (`-n`) y binario: el LED y su temporizador **no** cambian; el monitor muestra `[LED] Mensaje ignorado…`.
6. **No bloqueo**: con eventos RFID/simulados fluyendo, comprobar que siguen apareciendo `[EVENTO RFID]`, ACK y `[MQTT] Publicado…` mientras los LEDs actúan; y cortar la red celular con un LED encendido: debe apagarse a los 2 s aunque MQTT esté reconectando.
7. Verificar con el esquema que GPIO18/19 no estén ligados a otro periférico de la T-SIM7070G (el firmware solo los usa como salida).
8. Registrar los resultados aquí.

## Resultados en hardware (LEDs)
_Pendiente de ejecutar._
