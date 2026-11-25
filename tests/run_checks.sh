#!/usr/bin/env bash
# Validaciones sin hardware. Solo lectura sobre el repo (compila en directorios temporales).
# Uso: bash tests/run_checks.sh   (desde la raíz del repo)
set -euo pipefail
cd "$(dirname "$0")/.."

CLI="${ARDUINO_CLI:-$LOCALAPPDATA/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe}"
GPP="${RV_GPP:-$LOCALAPPDATA/Arduino15/packages/esp32/tools/esp-rv32/2601/bin/riscv32-esp-elf-g++.exe}"
FQBN="${FQBN:-esp32:esp32:esp32c6:ZigbeeMode=zczr,PartitionScheme=zigbee_zczr}"
TMP="${TEMP:-/tmp}"

echo "== 1. Copias de cabeceras compartidas idénticas"
cmp EmisorC6/rfid_zigbee_protocol.h CoordinadorC6/rfid_zigbee_protocol.h
cmp EmisorC6/rfid_zigbee_ep.h CoordinadorC6/rfid_zigbee_ep.h
cmp CoordinadorC6/trace_link.h LilyGoReceptor/trace_link.h
echo OK

echo "== 2. Protocolo y parser (static_assert)"
"$GPP" -std=gnu++17 -Wall -Wextra -fsyntax-only tests/protocol_selftest.cpp
"$GPP" -std=gnu++17 -Wall -Wextra -fsyntax-only -fconstexpr-ops-limit=400000000 -fconstexpr-loop-limit=10000000 tests/trace_link_selftest.cpp
"$GPP" -std=gnu++17 -Wall -Wextra -fsyntax-only -fconstexpr-ops-limit=400000000 -fconstexpr-loop-limit=10000000 tests/mqtt_json_selftest.cpp
echo OK

for s in EmisorC6 CoordinadorC6; do
  echo "== 3. Compilación $s ($FQBN)"
  "$CLI" compile --fqbn "$FQBN" --build-path "$TMP/bp_$s" --warnings default "$s" | tail -3
done

echo "== 4. Compilación LilyGoReceptor (esp32:esp32:esp32, ESP32 Dev Module)"
"$CLI" compile --fqbn "${FQBN_LILYGO:-esp32:esp32:esp32}" --build-path "$TMP/bp_LilyGoReceptor" --warnings default LilyGoReceptor | tail -3
