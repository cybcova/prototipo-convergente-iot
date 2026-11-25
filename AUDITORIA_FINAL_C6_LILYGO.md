# Auditoría final — integración Coordinador ESP32-C6 → LilyGO 7070

Fecha: 2026-11-24 · Alcance: solo lectura. No se modificó ningún firmware ni protocolo; este reporte es el único archivo nuevo. Sin commits, push ni despliegues.
Archivos revisados: `CoordinadorC6/CoordinadorC6.ino`, `CoordinadorC6/trace_link.h`, `LilyGoReceptor/LilyGoReceptor.ino`, `LilyGoReceptor/trace_link.h`, `Ejemplos/c6_lilygo/*`, `PROTOCOLO_C6_LILYGO.md`, `PRUEBAS_RFID_ZIGBEE.md`, `tests/*`.

Leyenda: **[COMPROBADO]** = evidente en código o reproducido con una prueba local · **[HIPOTÉTICO]** = plausible por el código/core, no ejecutado · **[PENDIENTE FÍSICO]** = requiere las placas.

---

## 1. Resumen
- El formato de línea, su checksum y sus límites están **bien construidos y probados localmente** (compilación + `static_assert`).
- La integración **no ofrece garantía de entrega**: es un enlace "enviar y olvidar" con *detección parcial de pérdidas en el receptor*. El ACK es solo un contador; **no hay estado de entrega por evento** y la detección de desconexión deja de funcionar tras el primer ACK (hallazgo H1).
- Ningún defecto comprobado impide la función básica (coordinador → línea validada → LilyGO). Nada de lo físico se ha probado.

## 2. Hallazgos por severidad

### Defectos comprobados
| ID | Sev. | Hallazgo | Evidencia |
|---|---|---|---|
| H1 | **ALTO** | **La pérdida del enlace deja de detectarse tras el primer ACK.** El único aviso (`Sin ACK…`) exige `linkAcked == 0`. Si el LilyGO se apaga o se desconecta después de haber enviado un solo ACK, el coordinador sigue contando `enviados` sin ninguna alerta (solo `enviados` vs `ack` en la línea de estado de 5 s, que nadie compara automáticamente). | `linkReadAcks()`: `if (linkSent >= 3 && linkAcked == 0 && …)`. |
| H2 | **ALTO** (limitación de diseño) | **No existe estado "enviado / recibido / validado" por evento.** "Enviado" = copiado al buffer TX de la UART (`Serial1.println` devuelve al encolar en el buffer, ni siquiera a salir por el pin). "Validado por LilyGO" = un contador `linkAcked`, sin asociarlo a un `seq`. El coordinador no sabe **qué** eventos llegaron. | `linkPump()`, `linkReadAcks()`: solo `linkSent++`, `linkAcked++`, `lastAckSeq = s`. |
| M1 | **MEDIO** | **El ACK no se valida contra lo enviado ni contra la sesión.** Un ACK duplicado, tardío, o con un `seq` nunca enviado (p. ej. mayor que `linkSeq`) incrementa `linkAcked` igualmente (puede llegar a `ack > enviados`) y sobrescribe `lastAckSeq` (que puede retroceder). El ACK `$ACK,<seq>*HH` **no lleva la sesión**: un ACK de una sesión anterior es indistinguible. | `linkReadAcks()`: no comprueba `s <= linkSeq`, ni deduplica, ni hay campo de sesión en `buildAck`. |
| M2 | **MEDIO** | **MSG ID no es único a lo largo del tiempo.** El emisor reinicia `nextId` en 1 al reiniciarse; el coordinador solo lo trata como reinicio si `msgId == 1` y no existe un identificador de arranque del emisor en el protocolo Zigbee. Además `lastId`/`haveLastId` son **globales** (un solo emisor). La clave (IEEE, MSG ID) puede repetirse entre arranques del emisor, y un MSG ID repetido tras reinicio puede parecer duplicado (`D`) o no detectarse. | Emisor: `uint16_t nextId = 1`; coordinador: `lastId` global; `msgId != 1` como excepción. |
| M3 | **MEDIO** | **Doc inexacta**: `PROTOCOLO_C6_LILYGO.md` dice que la ausencia de ACK "se nota" con aviso cada 60 s; eso solo es cierto hasta el primer ACK (H1). | Comparar doc con `linkReadAcks()`. |
| L1 | BAJO | **Checksum XOR de 8 bits**: detecta cualquier alteración de un solo carácter (verificado para todas las posiciones en `errors()`), pero **no detecta transposición de caracteres ni errores pareados**. Reproducido: intercambiar `E2E3` → `2EE3` en el EPC produce una línea **aceptada** con un EPC distinto. Adecuado para un cable corto; débil como garantía de integridad. | Prueba temporal fuera del repo (se eliminó): `parse()` devolvió OK con `epc[0]==0x2E`. |
| L2 | BAJO | **El log de descarte del anillo no identifica el evento**: `[LILYGO] Cola pendiente llena: descartado el evento mas antiguo (total…)` no imprime su `seq` ni MSG ID. (Sí es identificable a posteriori por el salto de `seq` en el LilyGO, y por `linkDropped`.) | `linkEnqueue()`. |
| L3 | BAJO | **Resincronización imperfecta del lector de ACK**: si la línea supera 48 bytes se hace `len = 0` pero se sigue acumulando el resto como si fuera una línea nueva hasta `\n` (no hay bandera de desbordamiento como en el LilyGO). Probabilidad de formar un ACK válido: despreciable (exigiría un `$ACK,…*HH` correcto). | `linkReadAcks()`. |
| L4 | BAJO | **`errByReason[]` del LilyGO se incrementa pero nunca se imprime** (código muerto). | `LilyGoReceptor.ino`. |
| L5 | BAJO | **`DIAG` es síncrono** (`log_printf` espera a vaciar UART0): `printEvent` emite ≈ 14 líneas (~100 ms) por evento dentro de `loop()`, retrasando `linkPump`/`linkReadAcks`. Sin pérdidas a las tasas actuales (RX de la UART1 tiene buffer), pero es latencia añadida. | Estructura de `printEvent`. |
| L6 | BAJO | Los tiempos `tRxMs`/`tTxMs` son `uint32` de `millis()`: reinician a ~49,7 días. Lo cubre la sesión solo si el coordinador se reinicia; no hay manejo de *wrap* (nadie los resta en el código actual). | `trace_link.h`. |

### Riesgos hipotéticos (no ejecutados)
| ID | Sev. | Riesgo |
|---|---|---|
| R1 | MEDIO | **IEEE atribuida al dispositivo equivocado si `addr_type` ≠ SHORT.** Si el stack entregara el origen con tipo IEEE, el callback deja `srcShort = 0` y `lookupIeee(0)` devolvería la IEEE **del coordinador** etiquetada como emisor. Los logs reales del usuario muestran `tipo de direccion 0` (SHORT), así que no se ha observado. No hay protección. |
| R2 | MEDIO | **`Serial1.availableForWrite()` puede devolver un valor optimista con el buffer TX lleno.** En el core 3.3.8, `uartAvailableForWrite` devuelve el espacio libre del *ring buffer*, **pero si ese espacio es 0 devuelve el espacio libre del FIFO (hasta 128)** (`available = txRingBufferAvailable == 0 ? available : txRingBufferAvailable`). Con el buffer de 1024 B completamente lleno, `linkPump` podría creer que cabe una línea y la escritura bloquearía. Requiere ≈ 9 líneas pendientes escritas de golpe (el anillo de 16 podría, en teoría, aportarlas); a las tasas reales no ocurre. |
| R3 | MEDIO | **Ráfagas en el LilyGO**: imprimir un evento (~700 B por USB a 115200 ≈ 60 ms) mientras llegan líneas de ~110 B; el buffer RX de 1024 B absorbe ≈ 8 líneas seguidas. Más de ~8 líneas en menos de ~0,5 s desbordarían el RX (se vería como `[ERROR RX]` y salto de `seq`). A 1 evento por 1–8 s no ocurre. |
| R4 | BAJO | **RX flotante en el LilyGO** (C6 desconectado/apagado): una entrada sin pull-up puede captar ruido y generar `[ERROR RX]` espurios. |
| R5 | BAJO | **Colisión de `sessionId`** (aleatorio de 32 bits, `esp_random()` tras iniciar Zigbee): 2⁻³² por reinicio; si ocurriera, el LilyGO no reiniciaría su seguimiento de `seq`. |
| R6 | BAJO | `esp_zb_ieee_address_by_short` **no se ha ejecutado nunca**; si la tabla NWK→IEEE estuviera obsoleta (dirección reutilizada tras la salida de un nodo) podría devolver otra IEEE. Si falla, el campo sale `-`. |

## 3. Respuestas a los puntos pedidos

### 3.1 ACK y secuencias
- **¿El ACK identifica inequívocamente la SEQ?** Dentro de una sesión, sí en cuanto a *valor* (`$ACK,<seq>*HH`, checksum validado). No identifica la **sesión** (M1) y el coordinador **no lo compara** con los `seq` realmente enviados.
- **¿Distingue enviado / recibido / validado?** No (H2). Solo contadores: `linkSent` (copiado al buffer TX), `linkAcked` (tramas ACK válidas recibidas). "Recibido por el LilyGO" y "validado por el LilyGO" coinciden: el LilyGO solo hace ACK de líneas que superan `parse()`; no hay ACK negativo.
- **ACK duplicado / tardío / de sesión anterior:** el coordinador lo cuenta como un ACK más (M1). No hay efecto sobre eventos porque no hay estado por evento. Es improbable en la práctica: el ACK se emite en milisegundos, y tras un reinicio del coordinador el buffer RX de la UART está vacío (reinicio de seguridad > 1 s).
- **¿Puede un ACK incorrecto marcar entregado un evento diferente?** **No**, porque ningún evento tiene marca de entrega. Sí puede **inflar contadores** y **silenciar el único aviso** (H1): un solo ACK válido, aunque sea espurio o de otra sesión, desactiva `Sin ACK` para siempre.
- LilyGO: responde ACK también a duplicados de `seq` y a líneas fuera de orden (consistente: son líneas válidas).

### 3.2 Cola y pérdidas
- **Anillo de 16 lleno:** `linkEnqueue` descarta el **más antiguo** (`linkHead` avanza, `linkCount--`), incrementa `linkDropped` e imprime un `DIAG`. El `seq` se asigna **antes** de encolar, por lo que el hueco es visible en el LilyGO como `SALTO DE SEQ: faltan N` **cuando llegue un evento posterior**. El log de descarte no nombra el `seq`/MSG ID descartado (L2).
- **¿Cuándo se llenaría?** Solo si `availableForWrite()` es insuficiente de forma sostenida o hay errores de serialización. Con la UART transmitiendo siempre (sin control de flujo), a las tasas actuales el anillo prácticamente **no** se llena; la ruta de descarte **no está cubierta por ninguna prueba** (ni host ni hardware).
- **UART sin receptor:** puede aceptar y "enviar" bytes indefinidamente sin que el firmware lo detecte. **[COMPROBADO]**: no hay RTS/CTS ni lectura de estado de línea; la UART vacía su buffer por el pin con independencia de que haya alguien escuchando. La única señal posible es la ausencia de ACK, y esa señal es ambigua (LilyGO apagado ≠ cable RX del ACK no conectado) y se pierde tras el primer ACK (H1).
- **Garantías reales:**
  - ✔ Un evento que llega al LilyGO se **valida** (formato, checksum, longitud, hex, rangos) o se rechaza con motivo; nunca se muestra una línea incompleta como válida.
  - ✔ El LilyGO puede **detectar** huecos y repeticiones de `seq` en los eventos que sí siguen llegando.
  - ✔ El coordinador **nunca bloquea Zigbee** por el LilyGO (callback intacto; escritura condicionada; anillo acotado), salvo el caso hipotético R2.
  - ✔ MSG ID, EPC, RSSI, origen e identificadores viajan sin modificarse.
- **Garantías que NO ofrece:**
  - ✘ Entrega: eventos escritos con el LilyGO apagado o desconectado **se pierden definitivamente** (sin reintento ni persistencia).
  - ✘ Detección de pérdida **al final de una racha**: si se pierden los últimos eventos y no llega otro, el LilyGO no puede notarlo.
  - ✘ Conocimiento, en el coordinador, de qué eventos llegaron (H2).
  - ✘ Orden o exactamente-una-vez entre sesiones.

### 3.3 Protocolo
- **Checksum:** XOR de los caracteres entre `$` y `*`, 2 dígitos hex. Probado: ida y vuelta, alteración de un carácter en **cada** posición, truncados en **cada** punto (`parse` nunca devuelve OK para un prefijo propio). Límite: L1 (transposiciones).
- **Delimitadores:** `$` inicio, `,` campos, `*HH` cierre, `\n` fin de línea (`\r` se ignora). Los campos son numéricos/hex/`R|S`/`V|D|G`: **no hay texto libre**, por lo que no se necesitan caracteres de escape; un `*`, `$` o `,` anómalo en un campo falla la conversión numérica o el conteo de campos (probado `BAD_FIELDS`, `BAD_NUMBER`, `BAD_HEX`).
- **Longitud máxima:** `LINE_LEN_MAX` = 224. Peor caso teórico de `build` = **180** caracteres (verificado con una prueba temporal con valores máximos: `static_assert(worst() == 180)`), con margen de 44. Constructor con `Writer` que nunca escribe fuera de `cap` y marca error.
- **Líneas incompletas:** truncadas → `NO_CHECKSUM`/`BAD_CHECKSUM`; sin `\n` durante 1 s → descartadas y resincronizadas; demasiado largas → se descartan hasta `\n` y se reporta `TOO_LONG`.
- **Desbordamientos:** LilyGO `rxBuf[LINE_LEN_MAX+1]` con guarda `rxLen < LINE_LEN_MAX`; coordinador `line[LINE_LEN_MAX+8]` y `build` con capacidad; ACK `buf[48]` con guarda. **No se encontraron escrituras fuera de rango.**
- **Conversión de campos:** `parseU` limita a 10 dígitos y usa `uint64` para detectar desbordes; `parseI` acota a ±1 000 000 y luego al rango; hex de longitud fija; EPC par, 1–32 bytes; probado con valores límite (`0xFFFFFFFF`, `0xFFFF`, −128, 127, −32768…). Acepta hex en minúsculas (el emisor siempre emite mayúsculas) y ceros a la izquierda (inofensivo).
- **Copias de `trace_link.h`:** **idénticas** (`cmp` y SHA-1 `a0462888c2e203238ecbf1091bc3ada88f3418e5` en ambas).

### 3.4 Trazabilidad
- **MSG ID y SEQ independientes: sí.** `out.msgId = ev.msg.msgId` (origen: emisor) y `ev.seq = ++linkSeq` (origen: coordinador, al encolar). Los tests usan valores distintos (570 vs 41) y el LilyGO los muestra por separado.
- **Reinicio del coordinador:** `linkSeq` vuelve a 0 y `sessionId` cambia (`esp_random()`); el LilyGO lo detecta (`Nueva sesion del coordinador`) y reinicia su seguimiento. `lastId`/`haveLastId` también se reinician (el primer evento tras reinicio sale `V`).
- **Reinicio del emisor:** MSG ID vuelve a 1; el coordinador no marca salto si llega 1; **no existe identificador de arranque del emisor** (M2).
- **Reinicio del LilyGO:** pierde su estado; el primer evento no puede evaluar huecos.
- **Tiempos:** el LilyGO los etiqueta `(uptime del coordinador)` / `(uptime del LilyGO)` y el comentario indica "no hora real"; el documento y la salida **no presentan ningún timestamp absoluto**. El coordinador imprime `a las %lu ms` sin la palabra "uptime" en el log `[LILYGO] Enviado…` (menor). `tRxMs` se toma en el callback (momento de entrega del stack), **no** es la hora de lectura RFID, que el emisor no envía.
- **IEEE del emisor, no del coordinador:** `srcIeee` se obtiene de `esp_zb_ieee_address_by_short(ev.srcShort)` con `srcShort` = dirección **origen** del comando (`message->info.src_address`), y `coordIeee` es un campo distinto (`esp_zb_get_long_address` propio). Correcto en el caso observado (tipo SHORT); riesgos R1 y R6. **No se ha ejecutado en hardware.**

### 3.5 Compatibilidad física
Configuración exacta en el código:

| Firmware | UART | Pines | Velocidad | Buffers |
|---|---|---|---|---|
| Ejemplo C6 | `Serial1` | TX = GPIO10, RX = GPIO11 | 115200 8N1 | por defecto |
| `CoordinadorC6` | `Serial1.begin(115200, SERIAL_8N1, rx=11, tx=10)` (orden de parámetros del core: `baud, config, rxPin, txPin`) | TX = GPIO10, RX = GPIO11 | 115200 8N1 | TX 1024 B (añadido) |
| Ejemplo LilyGO | `Serial2` | RX = GPIO32, TX = GPIO33 | 115200 8N1 | por defecto |
| `LilyGoReceptor` | `Serial2.begin(115200, SERIAL_8N1, rx=32, tx=33)` | RX = GPIO32, TX = GPIO33 | 115200 8N1 | RX 1024 B (añadido) |
| Monitores | C6: logs/`Serial` por UART0 (GPIO16/17) y consola USB-JTAG; LilyGO: `Serial` USB | — | 115200 | — |

Mismos pines, velocidad y formato de trama UART que el ejemplo; GPIO10/11 no entran en conflicto con el resto del coordinador (no usa más GPIO).

- **Se puede afirmar del ejemplo original:** usa UART de hardware (no USB), 115200 8N1, pines indicados, texto por líneas `\n`, sin ACK/validación/recuperación.
- **No se puede afirmar / sigue sin comprobarse:**
  - El **esquema de la LilyGO T-SIM7070G**: si GPIO32/GPIO33 están libres o ligados al módem/SD/otros periféricos, y niveles lógicos en esos pines. El repo no contiene el esquema; no se verificó.
  - Que el ejemplo se ejecutó con éxito (el usuario lo indica; el repo no guarda resultados).
  - La **placa de Arduino IDE** de la LilyGO (el core 3.3.8 no trae definición específica; se compiló con `ESP32 Dev Module`).
  - Cableado: TX/RX cruzados, **GND común** (no aparece en el ejemplo) y si el cable `GPIO33→GPIO11` (ACK) existe físicamente.
  - Que GPIO10/11 del C6 estén accesibles/libres en la placa concreta (no son pines de strapping según la documentación del C6; verificar en el datasheet de la placa).

## 4. Pruebas ejecutadas (solo las existentes, `bash tests/run_checks.sh`)
| Categoría | Prueba | Resultado |
|---|---|---|
| Consistencia | Copias de `rfid_zigbee_protocol.h`, `rfid_zigbee_ep.h`, `trace_link.h` idénticas | PASA |
| **Formato** | `protocol_selftest.cpp` (protocolo Zigbee y parser YRM) y `trace_link_selftest.cpp` (línea, checksum, rechazos, ACK), por `static_assert` | PASA |
| **Compilación** | `EmisorC6` (692 858 B), `CoordinadorC6` (694 628 B), ambos `zczr`/`zigbee_zczr` | PASA, 0 advertencias |
| **Compilación** | `LilyGoReceptor` con `ESP32 Dev Module` (285 532 B) | PASA, 0 advertencias |
| Adicional (solo lectura, archivo temporal ya borrado) | Peor caso de longitud = 180; transposición no detectada por XOR | Confirmados |
| **Físicas** | Todas | **NO EJECUTADAS** |

**Lo que las pruebas locales NO cubren:** la lógica de `linkEnqueue/linkPump/linkReadAcks` (viven en el `.ino`, solo se compila), el seguimiento de `seq`/sesión del LilyGO (idem), el desbordamiento del anillo, el comportamiento de la UART con/sin receptor, la IEEE del emisor y cualquier temporización.

## 5. Limitaciones de entrega (resumen)
- Mejor esfuerzo, **sin reintento ni persistencia**; la pérdida solo es detectable a posteriori y no al final de una racha.
- El coordinador no sabe qué eventos llegaron; ACK = contador, sin sesión, sin deduplicación.
- Detección de enlace caído solo hasta el primer ACK.
- MSG ID no único entre reinicios del emisor; un solo emisor soportado en la detección de duplicados/saltos.
- Sin timestamp de lectura RFID; tiempos = uptime.

## 6. Procedimiento recomendado para las pruebas físicas
Orden pensado para aislar fallos de capa en capa; guardar los logs de las tres consolas.

0. **Antes de energizar:** revisar en el esquema de la LilyGO que GPIO32/33 estén libres; cablear C6 GPIO10→LilyGO GPIO32, **GND↔GND**, y (opcional) LilyGO GPIO33→C6 GPIO11. Ambos a 3,3 V.
1. **Ejemplo original sin cambios** (`c6_lilygo`): comprobar `Recibido: hola` cada segundo. Si falla, el problema es de cableado/pines y no del proyecto.
2. Cargar `LilyGoReceptor` y `CoordinadorC6`; emisor sin tocar. Esperar `Conectado al coordinador` y los `[SIM]` del emisor (modo simulado, 8 s).
3. **Camino feliz (≥ 10 min):** MSG ID y EPC idénticos en emisor, coordinador y LilyGO; `ORIGEN: SIMULADO`; `RSSI ZIGBEE`; `DISPOSITIVO ORIGEN` con NWK **e IEEE** (anotar si aparece IEEE o `-`, y comparar con la IEEE real del emisor; esto valida R1/R6); `CHECKSUM: OK`; `seq` consecutivo; en el coordinador `ack == enviados`.
4. **ACK:** con el cable GPIO33→GPIO11 quitado, el coordinador debe mostrar `Sin ACK` a los ~3 envíos; reconectarlo y anotar que **el aviso no se repite** (confirma H1).
5. **LilyGO apagado 1 min y reencendido** (cable puesto): Zigbee y logs del coordinador siguen; al volver, el LilyGO debe reportar `SALTO DE SEQ: faltan N`; anotar `enviados` vs `ack` del coordinador. Repetir **tras** haber recibido ACKs previos (confirma H1: no hay aviso).
6. **Cable TX del C6 desconectado/reconectado en marcha**, incluso a mitad de una línea: esperar `linea_incompleta` o `checksum_incorrecto`, y recuperación en la línea siguiente.
7. **Reinicios:** (a) coordinador: nueva sesión en el LilyGO, `seq` desde 1; (b) emisor: MSG ID vuelve a 1 (documenta M2); (c) LilyGO: sin falsos saltos en el primer evento.
8. **Ráfagas (opcional):** forzar varias lecturas reales seguidas (etiqueta alternando) y vigilar `[ERROR RX]` en el LilyGO (R3).
9. **Etiqueta real:** `ORIGEN: REAL`, EPC igual al impreso por el emisor.
10. Registrar resultados y pegar los logs en `PRUEBAS_RFID_ZIGBEE.md`.

## 7. Correcciones sugeridas (no implementadas)
1. (H1/H2/M1) Estado por evento en el coordinador (p. ej. `seq` pendiente de ACK con tiempo de espera), validar `ACK.seq ≤ linkSeq`, deduplicar y añadir la sesión al ACK; avisar mientras `enviados − ack` supere un umbral, no solo con `ack == 0`.
2. (M2) Añadir un identificador de arranque del emisor al protocolo Zigbee (requiere cambiar el protocolo; decisión del usuario).
3. (R1) Rechazar `ieeeKnown` cuando `addrType != SHORT`.
4. (R2) Reservar margen conservador antes de escribir, o contabilizar los bytes pendientes en el firmware.
5. (L1) Sustituir XOR por CRC-16 si se necesita mayor integridad.
6. (L2–L5) Registrar el `seq` descartado, bandera de desbordamiento en el lector de ACK, imprimir o eliminar `errByReason`, pull-up en el RX del LilyGO.
7. Corregir la documentación (M3).
