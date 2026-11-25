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
