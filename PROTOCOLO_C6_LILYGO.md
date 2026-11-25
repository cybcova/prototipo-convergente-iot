# Enlace serial Coordinador ESP32-C6 → LilyGO 7070 (trazabilidad RFID)

Estado: implementado y compilado; **no probado en hardware** (ver `PRUEBAS_RFID_ZIGBEE.md`).

## 1. Auditoría del ejemplo `Ejemplos/c6_lilygo` (solo 2 archivos, sin README ni esquema)

| # | Pregunta | Respuesta (evidencia: código) |
|---|---|---|
| 1 | Archivos | `c6/c6.ino` (12 líneas) y `lilyGo/lilyGo.ino` (17 líneas). Sin README, esquemas ni instrucciones; los únicos comentarios están en los propios `.ino`. |
| 2 | Código del C6 | Envía el texto `hola` cada segundo: `Serial1.println("hola"); delay(1000);` |
| 3 | Código del LilyGO | Lee líneas de `Serial2` y las muestra en el monitor con prefijo `Recibido: `. |
| 4 | Conexión física | No documentada. Se deduce de los pines: C6 TX (GPIO10) → LilyGO RX (GPIO32); C6 RX (GPIO11) ← LilyGO TX (GPIO33). Masa común (GND–GND) necesaria pero **no mencionada** en el ejemplo. Ambos a 3,3 V lógicos. |
| 5 | Interfaz | **UART por hardware** (comprobado en código: `Serial1.begin(115200, SERIAL_8N1, RX, TX)` y `Serial2.begin(..., SERIAL_8N1, RX, TX)`), no USB Serial. El monitor del LilyGO (`Serial`) sí es USB, pero es solo la consola. |
| 6 | GPIO | C6: `TX_PIN 10`, `RX_PIN 11` (UART1). LilyGO: `RX_PIN 32`, `TX_PIN 33` (UART2). |
| 7 | Velocidad | 115200 baudios, 8N1, en ambos lados. |
| 8 | Inicialización | `Serial1.begin(...)` en `setup()` del C6; `Serial.begin(115200)` + `Serial2.begin(...)` en el LilyGO, que imprime `LilyGO lista, esperando datos...`. Sin handshake. |
| 9 | Formato de datos | Texto ASCII, un mensaje por línea. `println` envía `\r\n`. |
| 10 | Interpretación | `Serial2.readStringUntil('\n')`: lee hasta `\n`, o hasta el timeout por defecto de `Stream` (1000 ms) y devuelve lo recibido hasta entonces. La cadena **conserva el `\r`**. No parsea ni valida nada. |
| 11 | Consola | `Serial.print("Recibido: "); Serial.println(msg);` |
| 12 | Dependencias | Solo el core Arduino-ESP32 (`Serial1/Serial2`, `String`). Ninguna biblioteca externa. |
| 13 | Configuración de placa | **No documentada.** C6: `ESP32C6 Dev Module` (por el comentario "ESP32-C6-DevKitC-1"). LilyGO (comentario "T-SIM7070G", ESP32 clásico): el core 3.3.8 instalado no tiene definición específica; se verificó compilación con `ESP32 Dev Module`. |

Mecanismos de confirmación / delimitación / validación / recuperación en el ejemplo:
- Delimitación: sí, `\n` (única).
- Confirmación (ACK): **no**. Validación (checksum, longitud, formato): **no**. Detección de pérdidas o duplicados: **no**.
- Recuperación: solo el timeout de 1 s de `readStringUntil`, que entrega una línea parcial **como si fuera válida**; `loop()` además se bloquea hasta 1 s en cada lectura.
- Tampoco hay control de flujo (RTS/CTS): si el receptor está apagado o desconectado, el C6 sigue escribiendo al vacío sin enterarse.

## 2. Qué se reutiliza y qué se añade

Reutilizado **sin cambios**: UART por hardware, 115200 8N1, pines (C6 UART1 TX=10/RX=11; LilyGO UART2 RX=32/TX=33), texto ASCII, un mensaje por línea con `println` (`\r\n`), monitor USB del LilyGO a 115200.

Añadido (compatible con lo anterior; una línea sigue siendo una línea):
- Prefijo `$`, checksum XOR `*HH` y número de secuencia `seq`, para validar que la línea está completa y detectar pérdidas/duplicados.
- Lectura **no bloqueante** en el LilyGO, con límite de longitud y timeout de línea incompleta.
- ACK opcional LilyGO→C6 por GPIO33→GPIO11.

## 3. Flujo
```
Emisor C6 --Zigbee 0xFC10--> Coordinador C6 --UART1 (GPIO10)--> LilyGO UART2 (GPIO32) --USB--> monitor
                                   ^------- ACK opcional (GPIO11) <------- (GPIO33) ---------+
```
El emisor RFID no se comunica con el LilyGO; todo pasa por el coordinador. El protocolo Zigbee no cambió.

## 4. Formato de línea (v1)
```
$RFID,<ver>,<seq>,<msgId>,<R|S>,<EPC hex>,<rssiLector>,<rssiZigbee>,<nwk hex4>,<ieee hex16|->,<coord ieee hex16|->,<sesion hex8>,<tRxMs>,<tTxMs>,<estado>*<HH>\r\n
```
Ejemplo: `$RFID,1,41,570,S,E2E3,-45,-35,1CB3,-,-,DEADBEEF,577671,577690,V*<HH>`

| Campo | Significado |
|---|---|
| `ver` | 1 |
| `seq` | Secuencia **del enlace serial**, asignada por el coordinador **al encolar** (empieza en 1 en cada arranque). Un descarte en el coordinador aparece como salto de `seq`. |
| `msgId` | **MSG ID original** del emisor, sin modificar. |
| `R`/`S` | Origen REAL / SIMULADO (`FLAG_SIMULATED`). |
| `EPC hex` | EPC completo (1..32 bytes). |
| `rssiLector` | RSSI del lector (byte crudo como int8; unidad no verificada). |
| `rssiZigbee` | RSSI del enlace Zigbee medido en el coordinador. |
| `nwk` | Dirección NWK del emisor (**puede cambiar** si se reasocia). |
| `ieee` | IEEE del emisor, MSB primero (identificador estable), obtenida con `esp_zb_ieee_address_by_short`; `-` si no se pudo resolver. |
| `coord ieee` | IEEE del coordinador (`esp_zb_get_long_address`). |
| `sesion` | Número aleatorio de arranque del coordinador: permite saber que `seq`/tiempos se reiniciaron. |
| `tRxMs` | `millis()` del coordinador cuando el stack **entregó** el mensaje Zigbee (callback). |
| `tTxMs` | `millis()` del coordinador al **serializar** la línea, justo antes de escribirla por UART. |
| `estado` | `V` válido; `D` MSG ID repetido en Zigbee (duplicado); `G` válido tras un salto de MSG ID (posible pérdida en Zigbee). |
| `*HH` | XOR de todos los caracteres entre `$` y `*`, en hexadecimal. |

Los tiempos son **uptime del coordinador**, no hora real. **No existe timestamp de lectura RFID**: el emisor no lo envía; `tRxMs` es cuándo el coordinador recibió el mensaje.

ACK (LilyGO → C6): `$ACK,<seq>*<HH>\r\n`, solo si la línea se validó. Es informativo: el coordinador no reintenta ni depende de él.

Para un EPC de 32 bytes la línea mide ≈ 190 caracteres; límite `LINE_LEN_MAX` = 224.

## 5. Implementación
- **`CoordinadorC6/CoordinadorC6.ino`**: el callback Zigbee **no cambia** (solo copia a su cola con espera 0). En `loop()`, cada evento válido se completa con IEEE/estado/tiempos y entra a un **anillo de 16** pendientes; `linkPump()` escribe solo si `Serial1.availableForWrite()` alcanza (TX buffer de 1024 B), sin esperas. Los logs `[DIAG]`/`[RFID]` previos se mantienen. El estado periódico (5 s) añade enviados/ACK/pendientes/descartados.
- **`LilyGoReceptor/LilyGoReceptor.ino`** (nuevo): lectura no bloqueante carácter a carácter; descarta `\r`; límite de longitud; timeout de 1 s para líneas incompletas; valida y muestra el evento con el formato de trazabilidad; cuenta errores, saltos, duplicados y fuera de orden; ACK.
- **`trace_link.h`** (copias idénticas en `CoordinadorC6/` y `LilyGoReceptor/`): construcción y validación de líneas, `constexpr`, probado con `static_assert` en `tests/trace_link_selftest.cpp`.

## 6. Confiabilidad
| Situación | Comportamiento |
|---|---|
| LilyGO apagado o cable desconectado | Zigbee y logs del coordinador siguen funcionando. La UART escribe al vacío (no hay contrapresión), **esos eventos se pierden en el enlace serial**. Se nota: no llegan ACK → aviso `[LILYGO] Sin ACK…` cada 60 s y `ack` estancado en el estado periódico. No hay reintento (v1). |
| Cable desconectado a mitad de línea | El LilyGO descarta el fragmento tras 1 s (`linea_incompleta`) y resincroniza en el siguiente `\n`; además el checksum rechazaría una línea truncada. |
| Varios EPC seguidos | Línea ≈ 13–15 ms a 115200; el anillo (16) y el TX buffer (1024 B) absorben ráfagas muy superiores a la tasa del emisor (simulación 1 cada 8 s; lector real limitado por la deduplicación de 1 s). |
| UART ocupada | `linkPump()` no escribe si no hay espacio y reintenta en la siguiente vuelta; nunca bloquea. Si el anillo se llena se descarta el más antiguo y el LilyGO lo ve como salto de `seq`. |
| Mensajes incompletos o corruptos | `$`, longitud máxima, `*HH`, conteo de campos, hex y rangos: se rechazan con motivo y no se hace ACK. |
| Duplicados | Del enlace: mismo `seq` repetido. De Zigbee: `estado=D` (MSG ID repetido), que se reenvía marcado, no se oculta. |

Limitaciones: sin reintentos ni persistencia (por diseño de esta versión); un único contador de MSG ID global en el coordinador (válido con un solo emisor); IEEE de origen depende de que el stack conserve la tabla NWK→IEEE (si no, `-`); GPIO32/33 del LilyGO y GPIO10/11 del C6 se tomaron del ejemplo y no se comprobaron contra el esquema de las placas.
