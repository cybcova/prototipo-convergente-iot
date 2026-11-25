# Protocolo RFID sobre Zigbee (v1)

Estado: implementado y compilado; **no probado en hardware** (ver `PRUEBAS_RFID_ZIGBEE.md`).

## Mecanismo elegido
**Comando de cluster personalizado ZCL** (cluster `0xFC10`, rango manufacturer-specific), enviado por **unicast directo** del emisor a la dirección corta `0x0000` (coordinador), endpoint 1. Sin binding.

Por qué:
- La librería Zigbee del core 3.3.8 expone `esp_zb_zcl_custom_cluster_cmd_req()` (envío) y `ZigbeeEP::onCustomClusterCommand()` (recepción); ambas verificadas en las cabeceras instaladas.
- Un EPC (12 B típico) no cabe en un atributo numérico estándar ni en On/Off/Level; un atributo cadena requeriría escrituras/reportes con binding y semántica de "estado", no de "evento". Un comando es un evento puntual.
- El coordinador es siempre `0x0000`; evita depender del binding (que en el ejemplo se crea solo mientras la red está abierta).
- Se conserva del ejemplo: canal 11, EP1 (coordinador), EP10 (emisor), `setRebootOpenNetwork(180)`, `begin(ZIGBEE_COORDINATOR)`.
- No se usa Toggle ni el cluster On/Off.

| Elemento | Valor |
|---|---|
| Perfil | HA `0x0104` |
| Cluster | `0xFC10` — servidor en coordinador, cliente en emisor |
| Atributo | `0x0000` U8 solo lectura = versión de protocolo (informativo) |
| Comando | `0x01` TAG_REPORT, dirección cliente→servidor |
| Destino | `0x0000`, endpoint 1 |
| Tipo de datos | Octet string ZCL |

## Formato del payload (octet string)
Todos los enteros con endianness explícito; sin structs binarias en el cable.

| Offset | Tamaño | Campo | Notas |
|---|---|---|---|
| 0 | 1 | `len` | longitud del cuerpo (= 6 + epcLen) — prefijo del octet string ZCL |
| 1 | 1 | `version` | `1` |
| 2 | 2 | `msgId` | uint16 little-endian; contador del emisor, wrap natural, inicia en 1 |
| 4 | 1 | `rssi` | int8 — byte crudo del lector (ver limitaciones) |
| 5 | 1 | `flags` | bit0 = RSSI válido |
| 6 | 1 | `epcLen` | 1..32 |
| 7 | epcLen | `epc` | bytes del EPC tal cual |

Tamaño: 7 + epcLen bytes (EPC de 12 B → 19 B). Máximo 39 B: cabe en un solo frame APS sin fragmentación.

## Validación (receptor, `rfidzb::decode`)
Rechaza y cuenta (no imprime desde el callback): puntero nulo/vacío; prefijo mayor que los bytes recibidos; cuerpo menor que la cabecera; versión ≠ 1; `epcLen` 0 o > 32; `len ≠ 6 + epcLen`. También rechaza cluster o comando distintos. El emisor no envía EPC vacíos o > 32 B ni tramas RFID inválidas (el parser ya descarta checksum/terminador/longitud erróneos).

## Confirmación de entrega
Tres niveles distintos; no confundirlos:
1. **`sendTag()` devuelve true** = el stack aceptó encolar el comando. Mensaje del emisor: "Transmisión aceptada por el stack". No prueba entrega.
2. **Callback `esp_zb_zcl_command_send_status_handler_register`** = estado de envío asíncrono (resultado de la transmisión APS, con ACK APS en unicast). Mensaje: "Entrega confirmada por la red" / "Entrega FALLIDA". Es confirmación de capa de red/APS, no de que la aplicación del coordinador procesó el mensaje. **Semántica exacta no verificada en hardware.**
3. **Evidencia definitiva**: el mismo EPC e id aparecen en el Serial del coordinador.
No hay reintentos de aplicación ni ACK de aplicación en v1. El `msgId` permite al coordinador detectar saltos (mensajes perdidos).

## Eventos y concurrencia
- Emisor: `loop()` lee UART/parsea; EPC nuevos (deduplicados 1 s) → cola de 8 (si se llena, descarta el más antiguo); `zigbeeTask` inicia Zigbee, espera conexión y envía. El callback de estado solo encola; la tarea imprime.
- Coordinador: el callback (tarea del stack Zigbee) decodifica y encola (16); `loop()` imprime. Nada bloquea en callbacks.

## Limitaciones conocidas
- **RSSI del lector**: se transmite el byte crudo como int8 y se rotula "dBm" como hacía el firmware original; la unidad real del YRM10002 no está verificada. El coordinador imprime además el RSSI del enlace Zigbee (distinto).
- Sin cifrado de aplicación (solo seguridad de red Zigbee por defecto); sin autenticación del origen.
- Sin fragmentación: EPC máx. 32 B.
- Deduplicación solo contra la última etiqueta (varias etiquetas alternando no se deduplican entre sí).
- Sin reintentos ni persistencia: si no hay red, los EPC se descartan.
- Verificado en hardware (2026-11-24): `data.size` incluye el byte de prefijo (EPC de 12 B → `size=19`, primer byte 18), así que `decode` es correcto.
- Las cabeceras `rfid_zigbee_protocol.h` y `rfid_zigbee_ep.h` están duplicadas por limitación de Arduino; `tests/run_checks.sh` verifica que sean idénticas.
