# 🌐 Práctica de Laboratorio: Red Zigbee con ESP32-C6 (Interruptor Maestro y Relevador)

Este repositorio contiene la guía paso a paso y el código fuente para implementar una red inalámbrica Zigbee básica utilizando microcontroladores ESP32-C6. El objetivo didáctico es comprender la arquitectura de nodos (Coordinador y Enrutador), el aislamiento de canales por software y el control de actuadores de corriente alterna mediante relevadores.

---

## 👨‍🏫 Datos de la Asignatura
* **Docente:** MGTI. Saul Isai Soto Ortiz

---

## 🛠️ 1. Lista de Materiales

Para llevar a cabo esta práctica por equipo, necesitarán:
* **2x Tarjetas de desarrollo ESP32-C6.** (Una será el "Apagador" y la otra el "Foco").
* **1x Módulo Grove Relay (HLS8L-DC3V-S-C).**
* **1x Foco (Bombilla) con su socket (portalámparas).**
* **1x Clavija con cable de corriente alterna (AC).**
* **Cables jumper (Dupont)** macho-macho y macho-hembra.
* Desarmador pequeño para las borneras del relevador.

---

## ⚠️ 2. Consideraciones Estrictas de Seguridad

**¡ATENCIÓN ESTUDIANTES!** En esta práctica manipularán voltaje de red eléctrica (110V/220V). Un error puede causar descargas eléctricas graves.
1. **Regla de Oro:** TODAS las conexiones de cables hacia el relevador y el foco deben realizarse con la clavija **DESCONECTADA** de la pared.
2. **Cero cables sueltos:** Asegúrense de que al atornillar los cables en la bornera verde del relevador, no quede ningún filamento de cobre expuesto.
3. **Inspección Docente:** Ningún equipo tiene permitido conectar su clavija a la toma de corriente del laboratorio sin que el Mtro. Saul haya inspeccionado visualmente su circuito.

---

## 🔌 3. Diagrama de Conexiones

### Nodo Actuador (El Foco)
**A. Control (ESP32-C6 al Grove Relay):**
* ⬛ **Cable Negro (GND):** Al pin **GND** del ESP32.
* 🟥 **Cable Rojo (VCC):** Al pin **3V3** (o **5V/VIN** si el relay no hace clic) del ESP32.
* ⬜ **Cable Blanco (NC):** No conectado.
* 🟨 **Cable Amarillo (Señal):** Al pin **GPIO 4** del ESP32.

**B. Potencia (Relay al Foco):**
1. El hilo **Neutro** de la clavija va directo al socket del foco.
2. Corten el hilo de **Fase** de la clavija.
3. Conecten la punta que viene de la **clavija** al tornillo del relay marcado con una **"C"** (COM).
4. Conecten la punta que va hacia el **foco** al tornillo marcado con **"NO"** (Normally Open).

### Nodo Coordinador (El Apagador)
Para evitar que la placa se bloquee al arrancar, NO usaremos el botón físico BOOT.
* Conecten un cable jumper al pin **GPIO 18**.
* Dejen la otra punta suelta. Para simular el botón, tocarán brevemente un pin **GND** con esa punta.

---

## ⚙️ 4. Configuración del Arduino IDE

Abran el menú **Herramientas (Tools)** y configuren:
1. **Board:** ESP32C6 Dev Module
2. **Partition Scheme:** `Zigbee 4MB with spiffs`
3. **Erase All Flash Before Sketch Upload:** `Enabled`

**Selección del Rol Zigbee:**
* Para el **Coordinador**, seleccionen: `Zigbee mode: Zigbee ZCZR (Coordinator/Router)`.
* Para el **Actuador (Foco)**, seleccionen: `Zigbee mode: Zigbee Router`. *(Nunca usen End Device o el foco se dormirá).*

---

## 🧪 5. Protocolo de Pruebas (La Regla de los 30 Segundos)

Para que las placas se reconozcan (Binding), el orden de encendido es vital:
1. Suban el código a ambas placas. Modifiquen la variable `canal_asignado` al número de su equipo (11-26).
2. Desconecten ambas placas.
3. Conecten **PRIMERO** la placa del Coordinador. Abran su Monitor Serial y esperen el mensaje de `Ventana de emparejamiento ABIERTA`.
4. Conecten **SEGUNDO** la placa del Actuador. Abran su Monitor Serial y verán que dice `Conectado a la red`.
5. **¡NO TOQUEN NADA!** Esperen pacientemente **30 segundos** para el intercambio de llaves de seguridad.
6. Toquen el pin GND con el cable conectado al pin 18 del Coordinador.
7. ¡El relevador hará "Clic" y el foco se encenderá!
