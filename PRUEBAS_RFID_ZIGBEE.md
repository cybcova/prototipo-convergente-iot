# Pruebas RFID + Zigbee

Fecha: 2026-11-24. Reproducir: `bash tests/run_checks.sh`.

## Resultados sin hardware

| # | Prueba | Resultado | Evidencia |
|---|---|---|---|
| 1 | Compilación `EmisorC6` (core esp32 3.3.8, `ZigbeeMode=zczr`, `PartitionScheme=zigbee_zczr`) | **PASA**, 0 advertencias | 691 932 B (52 %), 35 608 B RAM estática |
| 2 | Compilación `CoordinadorC6` (mismas opciones) | **PASA**, 0 advertencias | 690 292 B (52 %), 35 360 B RAM |
| 3 | Compilación `EmisorC6` con partición `zigbee` (la del README) | **PASA** | 691 924 B |
| 4 | Compilación `CoordinadorC6` con partición `zigbee` | **No completada** (se interrumpió por tiempo) | — |
| 5 | Protocolo: ida y vuelta (EPC 1/12/32 B, RSSI −128..127, id 1..0xFFFF), layout en el cable, rechazos de encode/decode (truncado, versión, epcLen 0/>32, longitud inconsistente, nulo) | **PASA** (`static_assert`) | `tests/protocol_selftest.cpp` |
| 6 | Parser YRM: trama válida, ruido previo, tramas consecutivas, checksum malo, terminador malo, longitud 0xFFFF, PC con EPC mayor que la trama, PC=0, trama de error 0x15, checksum del comando de inventario (0x83) | **PASA** | idem |
| 7 | Las pruebas pueden fallar (mutación temporal de un byte esperado y del cálculo de checksum) | Fallan como se espera (1 y 5 aserciones) | mutación en directorio temporal |
| 8 | Cabeceras duplicadas idénticas | **PASA** | `cmp` |
| 9 | Revisión estática de APIs | Todas existen en el core 3.3.8 | `esp_zb_zcl_custom_cluster_cmd_req`, `esp_zb_zcl_command_send_status_handler_register`, `esp_zb_cluster_list_add_custom_cluster`, `esp_zb_custom_cluster_add_custom_attr`, `esp_zb_lock_acquire(ticks)`, `ZigbeeEP::onCustomClusterCommand` |
| 10 | Revisión de callbacks | No imprimen ni bloquean: solo encolan con espera 0; contadores con un único escritor | código |

Las pruebas 5–6 se evalúan en compilación porque el entorno no tiene compilador host; no se ejecutó código.

## NO ejecutado (requiere hardware)
- Unión del emisor a la red, envío real, recepción real, formato exacto de `data.size` en recepción, semántica del callback de estado de envío, comportamiento real del rol (End Device explícito = el default que ya usaba el actuador), lectura real del YRM10002 con este firmware, dedupe/silencio del lector, estabilidad prolongada, pérdida de red y recuperación.
- **No se puede afirmar que el sistema funciona físicamente.**

## Procedimiento manual de validación
Arduino IDE: Board `ESP32C6 Dev Module`; Zigbee mode `Zigbee ZCZR (coordinator/router)`; Partition Scheme igual a la usada en el ejemplo que funcionó (si no recuerdas: `Zigbee ZCZR 4MB with spiffs`); Erase All Flash `Enabled` en la primera carga de cada placa; Serial 115200.

1. Carga `CoordinadorC6/CoordinadorC6.ino` en la placa A. Abre su monitor.
2. Reinicia A. Espera `[ZIGBEE] Coordinador iniciado` y `Red disponible` (ventana 180 s).
3. Carga `EmisorC6/EmisorC6.ino` en la placa B (lector YRM10002 conectado: RX=GPIO4, TX=GPIO5). Reiníciala dentro de la ventana.
4. En B espera `[ZIGBEE] Conectado al coordinador`. Si no conecta en 180 s, B se reinicia sola.
5. Acerca una etiqueta. Verifica en B: `[RFID] EPC detectado: <HEX>`, `Enviando RFID (id N)`, `Transmision aceptada`, y luego `Entrega confirmada por la red`.
6. Verifica en A: `Dispositivo incorporado`, `[RFID] Mensaje recibido`, mismo EPC y mismo id.
7. Criterio de éxito: EPC de A == EPC de B para ≥ 20 lecturas distintas (alejar/acercar la etiqueta), sin saltos de id.
8. Si A muestra `Mensaje rechazado: motivo=2 size=… primer_byte=…`, copia esa línea (hipótesis del prefijo, ver protocolo).
9. Apaga A con B encendido: B debe seguir leyendo y mostrar "Sin red"/"Entrega FALLIDA"; vuelve a encender A y comprueba recuperación (puede requerir el reinicio automático de B).
10. Pega los logs de ambos monitores para registrar resultados aquí.

## Resultados en hardware

### 2026-11-24 — modo simulado, dos ESP32-C6 (evidencia pegada por el usuario)
- **PASA: EPC simulado de extremo a extremo.** El coordinador imprimió `MSG ID: 570/571`, `EPC: E28068940000501234567AC9/…ACA (12 bytes)`, `RSSI: -47/-49`, `RSSI enlace Zigbee: -35/-38 dBm`, `ORIGEN: SIMULADO`, con `validacion: OK`. El emisor había confirmado entregas (`ok:18 fallo:0` en una captura anterior). El sistema llevaba ~578 s en marcha (id ≈ 570, un mensaje por segundo).
- Hipótesis del prefijo **descartada**: el stack entrega `size=19` con `primer_byte=18` (= 1 + 18), es decir, `size` incluye el byte de longitud; `decode` es correcto.
- Origen reportado por el stack: dirección NWK (tipo 0); la IEEE no viene en estos mensajes.
- Causa de que antes no se vieran los `[RFID]`: canal de salida (logs por consola ROM/USB-JTAG vs `Serial` en UART0). Resuelto en el coordinador con `log_printf` (macro `DIAG`).
- **Limitaciones de esta evidencia:** solo se vieron 2 mensajes consecutivos (sin saltos de id) en el fragmento pegado; no se midió pérdida a largo plazo, ni recuperación tras apagar un nodo, ni reinicios.
- **Sigue SIN probarse:** lectura de etiquetas reales con el YRM10002 (`RFID_SIMULATION_MODE = false`), que el usuario reporta con dificultades de detección en la antena; ese problema es de lector/antena y no está relacionado con el enlace Zigbee.

---

# Modo de simulación (sin lector RFID)

Sirve para probar Zigbee extremo a extremo sin depender del YRM10002.

## Activación
En `EmisorC6/EmisorC6.ino`, al inicio:
```cpp
constexpr bool RFID_SIMULATION_MODE = true;   // true = además de lo real, genera simulados; false = solo real
constexpr unsigned long SIM_INTERVAL_MS = 8000;
```
Cambiar solo `RFID_SIMULATION_MODE` y recompilar. **Desde el cambio de arquitectura (abajo) la lectura real del YRM10002 está SIEMPRE activa; la constante solo añade o quita los EPC simulados.** (Las secciones anteriores describen la versión previa, donde eran excluyentes; las salidas de ejemplo siguientes son de esa versión.)

## Qué hace
- Cada 8 s (`SIM_INTERVAL_MS = 8000`; antes 1000 ms, para dejar tiempo a probar con etiquetas reales) (por `millis()`, sin `delay()` bloqueante; solo cuando el emisor ya está conectado) genera un EPC de 12 bytes: `E28068940000501234567890`, `...91`, `...92`... (los 2 últimos bytes son un contador de 16 bits) y RSSI `-45, -47, ... -59` en ciclo. Indefinidamente.
- Lo mete en **la misma cola** (`tagQueue`) que los EPC reales; `zigbeeTask` lo envía con el mismo `sendTag()` (protocolo v1, cluster `0xFC10`, cmd `0x01`, destino `0x0000` EP1) y los mismos callbacks de estado de envío. No hay segundo mecanismo de transmisión.
- Marcador de simulación: **bit1 (`FLAG_SIMULATED = 0x02`) del byte `flags` ya existente** en el protocolo. No cambia el formato ni el tamaño del mensaje ni toca el EPC (la versión sigue siendo 1; un coordinador antiguo ignoraría el bit).
- El `msgId` lo asigna `zigbeeTask` al enviar (consecutivo).
- ~~En modo simulado no se envía el inventario~~ (obsoleto: ahora el lector real siempre está activo).

## Salida esperada
Emisor:
```
====================================
       RFID SIMULATION MODE
====================================
...
[SIM] Generando RFID ficticio
[SIM] EPC: E28068940000501234567890
[SIM] RSSI: -45
[SIM] MSG ID: 1
[ZIGBEE] Enviando mensaje...
[ZIGBEE] Transmision aceptada por el stack (entrega pendiente de confirmacion)
[ZIGBEE] Entrega confirmada por la red (tsn N) | ...
```
Coordinador:
```
[RFID] Mensaje recibido
[RFID] MSG ID: 1
[RFID] EPC: E28068940000501234567890 (12 bytes)
[RFID] RSSI: -45 dBm (del lector, valor crudo como int8)
[RFID] RSSI enlace Zigbee: ...
[RFID] ORIGEN: SIMULADO (emisor 0x....)
```
(Cambios en el coordinador: etiqueta `MSG ID`, y línea `ORIGEN` que distingue SIMULADO de lector real. Nada más.)

## Procedimiento
1. Carga `CoordinadorC6` en la placa A y arráncala (ventana de unión 180 s). Mismas opciones de IDE que arriba.
2. Carga `EmisorC6` (con `RFID_SIMULATION_MODE = true`) en la placa B. No necesita lector conectado.
3. Espera `[ZIGBEE] Conectado al coordinador` en B; a partir de ahí aparece un `[SIM]` cada 8 s.
4. Criterio de éxito: en A llega cada mensaje con el mismo EPC y MSG ID que en B, `ORIGEN: SIMULADO`, sin avisos de salto de id durante varios minutos.
5. Si A no muestra nada pero B dice "Entrega confirmada", o A muestra `Mensaje rechazado: motivo=…`, copia esas líneas.

## Validación sin hardware (2026-11-24)
| Prueba | Resultado |
|---|---|
| Compila `EmisorC6` (sim=true) y `CoordinadorC6`, 0 advertencias | PASA (691 354 B y 690 368 B, 52 %) |
| Cabeceras de protocolo idénticas en ambos sketches | PASA (`cmp`) |
| Flag simulado: ida y vuelta, independiente de RSSI válido, EPC intacto | PASA (`static_assert`) |
| Secuencia EPC simulada (12 B, válida para `epcLen` 1..32) y RSSI -45..-59 | Revisada por código; los 12 B pasan `encode/decode` |
| Modo real disponible | La rama `false` compila siempre (es un `if` sobre constante, no un `#if`); **no ejecutada con `false` en esta sesión** |
| Zigbee/Recepción en hardware | **NO probado** |

---

# Lectura real permanente + simulación opcional (cambio de arquitectura)

| `RFID_SIMULATION_MODE` | RFID real | RFID simulado |
|---|---|---|
| `false` | Activo permanentemente | Desactivado |
| `true` | Activo permanentemente | Activo cada `SIM_INTERVAL_MS` = 8000 ms |

## Cambios en `EmisorC6/EmisorC6.ino` (único archivo de firmware modificado)
- `setup()`: UART1, comando de inventario y mensajes de lector ya no dependen del booleano. El booleano solo decide qué se imprime sobre la simulación.
- `loop()`: se eliminó el `return` anticipado del modo simulado; el bucle UART + parser + reanudación por silencio se ejecuta siempre. El bloque de simulación está al final y solo se evalúa si el booleano es `true`.
- Cola: sigue habiendo un único productor (`loop()`: reales y simulados) y un único consumidor (`zigbeeTask`); las colas de FreeRTOS son seguras entre tareas. Con la cola llena, un EPC **real** desplaza al más antiguo, pero un EPC **simulado** se descarta a sí mismo, de modo que la simulación nunca expulsa lecturas reales.
- Deduplicación (1 s): solo en `onTagDetected()` (lecturas reales); la simulación no la toca ni modifica `lastEpc`.
- `msgId`: contador único en `zigbeeTask`, compartido por reales y simulados (consecutivos entre ambos tipos).
- Salida en el emisor (Serial): `[RFID] Lector YRM10002 inicializado`, `[RFID] Lectura real: ACTIVA`, `[SIM] Simulacion: ACTIVADA/DESACTIVADA`, `[SIM] Intervalo: 8000 ms`, `[ZIGBEE] Inicializando...`; por lectura real `[RFID REAL] EPC detectado: …` + `[ZIGBEE] Encolando lectura real`; por simulada `[SIM] EPC ficticio generado: …` + `[ZIGBEE] Encolando lectura simulada`; al enviar `[ZIGBEE] Enviando mensaje REAL|SIMULADO (MSG ID n) EPC …`.
- Coordinador, protocolo, parser, GPIO, roles y red: sin cambios. El coordinador distingue por `FLAG_SIMULATED`; imprime `ORIGEN: SIMULADO` o `ORIGEN: lector RFID real, ...` (el texto exacto es ese último, no `LECTOR RFID REAL`).

## Validación sin hardware
| Prueba | Resultado |
|---|---|
| A. Compila con `RFID_SIMULATION_MODE = true` (0 advertencias) | PASA (692 858 B, 52 %) |
| B. Compila con `RFID_SIMULATION_MODE = false` (copia temporal fuera del repo, 0 advertencias) | PASA (692 336 B) |
| Revisión de código: `RFID_SIMULATION_MODE` aparece solo en 4 sitios (definición, 2 impresiones de arranque en `setup()` y el bloque de simulación al final de `loop()`); ningún condicional protege `RFID.begin`, el comando de inventario, el parser ni la reanudación | PASA |
| `tests/run_checks.sh` (cabeceras idénticas y static_assert de protocolo/parser) | PASA |
| Lectura real con el YRM10002 | **NO probada**: no se ha verificado físicamente que el lector detecte etiquetas con este firmware (el usuario reporta dificultades con la antena) |
| Simulación cada 8 s tras este cambio, y convivencia real+simulado en hardware | **NO probada** tras el cambio (la simulación sí se validó antes, ver arriba) |

## Procedimiento sugerido
1. Coordinador cargado y corriendo (sin cambios).
2. Cargar `EmisorC6` con `RFID_SIMULATION_MODE = true`, lector conectado. Esperar `Conectado al coordinador`.
3. Cada 8 s debe llegar un mensaje `ORIGEN: SIMULADO` al coordinador; al acercar una etiqueta debe aparecer `[RFID REAL] EPC detectado` en el emisor y un mensaje con `ORIGEN: lector RFID real` en el coordinador. Si no aparece, el problema está antes de Zigbee (lector/antena/cableado).
4. Repetir con `false`: no debe haber mensajes `[SIM]` ni `ORIGEN: SIMULADO`.

---

# Enlace Coordinador → LilyGO 7070 (trazabilidad)

Detalle de diseño y formato: `PROTOCOLO_C6_LILYGO.md`.

## Validación sin hardware (2026-11-24)
| Prueba | Resultado |
|---|---|
| `CoordinadorC6` compila (`zczr` / `zigbee_zczr`), 0 advertencias | PASA (694 628 B, 52 %) |
| `EmisorC6` (sin cambios) compila | PASA (692 858 B) |
| `LilyGoReceptor` compila con `esp32:esp32:esp32` (ESP32 Dev Module), 0 advertencias | PASA (285 532 B, 21 %) — placa exacta del ejemplo no documentada |
| Formato de línea: ida y vuelta (EPC 1/12/32 B, real/simulado, estados V/D/G, IEEE conocida/desconocida, valores límite), layout exacto, truncada en cualquier punto → rechazada, un byte alterado en cualquier posición → rechazado, versión/origen/EPC impar o vacío/hex/estado/campos/rangos inválidos, ACK | PASA (`static_assert`, `tests/trace_link_selftest.cpp`) |
| Las pruebas fallan si se rompe el checksum (mutación temporal) | Falla como se espera (6 aserciones) |
| Copias de `trace_link.h` idénticas | PASA (`cmp`) |
| Callback Zigbee sin impresiones ni UART | Revisado: sin cambios; solo copia a cola con espera 0 y `millis()` |
| **Enlace físico C6 ↔ LilyGO, ACK, IEEE del emisor, comportamiento con LilyGO apagado/desconectado, ráfagas** | **NO probado** |

## Cableado a verificar (deducido de `Ejemplos/c6_lilygo`, no documentado allí)
- C6 GPIO10 (TX) → LilyGO GPIO32 (RX)
- C6 GPIO11 (RX) ← LilyGO GPIO33 (TX) *(solo ACK; opcional)*
- GND del C6 ↔ GND del LilyGO (imprescindible; ambos 3,3 V lógicos)

## Procedimiento manual
1. Cargar `CoordinadorC6` en el C6 coordinador y `LilyGoReceptor` en el LilyGO (monitor 115200). Emisor sin recargar.
2. Con el cableado anterior, el LilyGO debe mostrar `RFID TRACEABILITY` / `esperando eventos…`.
3. Con el emisor enviando (simulado cada 8 s), el coordinador debe mostrar `[LILYGO] Enviado seq N …` y, si el cable RX está conectado, `[LILYGO] ACK seq N`. El LilyGO debe mostrar `[EVENTO RFID]` con el mismo MSG ID y EPC que el coordinador, `ORIGEN: SIMULADO`, `RSSI ZIGBEE`, `DISPOSITIVO ORIGEN` (NWK e IEEE) y `[TRANSPORTE SERIAL] ESTADO: RECIBIDO`, `CHECKSUM: OK`.
4. Criterio de éxito: MSG ID y EPC idénticos en emisor, coordinador y LilyGO durante varios minutos, sin `[ERROR RX]` ni saltos de SEQ.
5. Pruebas de fallo: (a) apagar el LilyGO 1 minuto: el coordinador debe seguir recibiendo por Zigbee y mostrar `Sin ACK`; al volver, los nuevos eventos llegan y el LilyGO debe reportar `SALTO DE SEQ: faltan N` (los eventos escritos mientras estaba apagado se pierden, pero el hueco en `seq` los delata; `ack` queda por debajo de `enviados` en el coordinador); (b) desconectar y reconectar el cable TX del C6 con el sistema en marcha; (c) acercar una etiqueta real y verificar `ORIGEN: REAL`.
6. Pegar los logs de las tres consolas para registrar resultados aquí.
