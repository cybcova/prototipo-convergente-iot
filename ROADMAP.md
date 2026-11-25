# ROADMAP — prototipo-convergente-iot

Propuesta priorizada derivada de `AUDITORIA_TECNICA.md`. **Nada de esto está implementado.** Los IDs (A1…) remiten a los hallazgos de la auditoría.

## 0. Prerrequisito: aclarar alcance
- [ ] Confirmar el objetivo (¿EPC por Zigbee a un coordinador? ¿y luego a dónde?).
- [ ] Confirmar placa, versión de core y cableado/alimentación del lector.

## 1. Correcciones necesarias (P0)
- [ ] A1: el Actuador debe solicitar rol Router explícito (`Zigbee.begin(ZIGBEE_ROUTER)`).
- [ ] A2/A3: corregir README (opciones reales del menú del core) y verificar la partición correcta (`Zigbee ZCZR 4MB` vs `Zigbee 4MB`) con prueba de arranque.
- [ ] Documentar en el repo versión del core y opciones de compilación (README raíz o `sketch.yaml`).
- [ ] Recuperar o reescribir el coordinador propio (commit `594dbf2`) con decisión consciente sobre ejemplo vs propio.

## 2. Estabilización del firmware (P1)
- [ ] A6/A7: timeouts y `ESP.restart()` en lugar de `while(1)`/esperas infinitas.
- [ ] A8: re-emitir inventario periódicamente o al agotarse; detectar silencio del lector.
- [ ] A9: parser RFID robusto (validar `epcLength`, resincronización, distinguir errores del lector, p. ej. "sin tag").
- [ ] A13: no imprimir desde el callback Zigbee; encolar y mostrar en `loop()`.
- [ ] A10: revisar pines de strapping y reasignar si hay arranques erráticos.
- [ ] Medir heap/stack libre y reset reason en logs.

## 3. Validación de la comunicación Zigbee (P1)
- [ ] Ejecutar el ejemplo de laboratorio completo y registrar logs (unión, binding, latidos, pérdida/recuperación).
- [ ] Prueba de reinicio sin "Erase All Flash": ¿la red y el binding persisten? (A5)
- [ ] Prueba de alcance/pérdida y recuperación del coordinador y del router.
- [ ] Confirmar rol efectivo de cada nodo (logs/scan de red) tras A1.

## 4. Integración de dispositivos (P2)
- [ ] Diseñar el modelo de datos del EPC sobre Zigbee (cluster/atributo personalizado, o cluster estándar apropiado), tamaño, rate-limit y deduplicación de lecturas.
- [ ] Nodo RFID+Zigbee (router o end device según energía) que combine `EmisorC6` con la librería Zigbee; evitar colisión de pines (A-GPIO4).
- [ ] Coordinador que reciba y exponga eventos (Serial inicialmente).
- [ ] Definir seguridad para uso real (A16): control de permit-join, llaves.

## 5. Pruebas funcionales (P2)
- [ ] Plan de pruebas con casos: lectura de varios tags, tag ausente, pérdida de enlace, reinicio de nodos, carga continua (horas).
- [ ] Bitácora de resultados en el repo.
- [ ] Pruebas de parser RFID con tramas grabadas (host, sin hardware).

## 6. Mejoras futuras (P3)
- Puerta de enlace hacia backend (MQTT/HTTP/Zigbee2MQTT u otro, sujeto a aprobación de tecnologías nuevas).
- Reporting configurado y bindings propios; OTA (las particiones lo permiten).
- Reducción de consumo si algún nodo pasa a batería (End Device).
- `.gitignore`, CI de compilación y licencias del material de terceros.
