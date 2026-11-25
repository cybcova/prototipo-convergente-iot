// Endpoint Zigbee con el cluster personalizado RFID (0xFC10). Ver PROTOCOLO_RFID_ZIGBEE.md.
//
// Misma advertencia que rfid_zigbee_protocol.h: hay DOS copias idénticas (EmisorC6/ y
// CoordinadorC6/). Solo usa APIs verificadas en las cabeceras del core esp32 3.3.8
// (ZigbeeEP, esp_zb_zcl_custom_cluster_cmd_req, esp_zb_custom_cluster_add_custom_attr).
#pragma once
#include "Zigbee.h"
#include "rfid_zigbee_protocol.h"

class ZigbeeRfidEP : public ZigbeeEP {
public:
  // serverRole=true  -> coordinador (recibe).  false -> emisor (envía; rol cliente).
  ZigbeeRfidEP(uint8_t endpoint, bool serverRole) : ZigbeeEP(endpoint) {
    _device_id = ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID;

    _cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(_cluster_list, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(_cluster_list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Cluster personalizado con un único atributo (versión de protocolo, solo lectura).
    // El atributo evita registrar un cluster vacío y permite diagnóstico por lectura.
    esp_zb_attribute_list_t *rfid = esp_zb_zcl_attr_list_create(rfidzb::CLUSTER_ID);
    esp_zb_custom_cluster_add_custom_attr(rfid, 0x0000, ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &protoVersionAttr_);
    esp_zb_cluster_list_add_custom_cluster(_cluster_list, rfid, serverRole ? ESP_ZB_ZCL_CLUSTER_SERVER_ROLE : ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    _ep_config = {.endpoint = _endpoint, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID, .app_device_version = 0};
  }

  // Envía un reporte de etiqueta por unicast (sin binding) al coordinador (0x0000, EP1).
  // true = el stack ACEPTÓ el comando para transmisión. NO significa que el coordinador lo recibió:
  // la entrega se confirma después, de forma asíncrona, por esp_zb_zcl_command_send_status_handler_register.
  bool sendTag(const rfidzb::TagMsg &msg, uint16_t dstShort = 0x0000, uint8_t dstEndpoint = rfidzb::COORD_ENDPOINT) {
    uint8_t wire[rfidzb::WIRE_MAX];
    const size_t n = rfidzb::encode(msg, wire, sizeof(wire));
    if (n == 0) return false;

    esp_zb_zcl_custom_cluster_cmd_req_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = dstShort;
    cmd.zcl_basic_cmd.dst_endpoint = dstEndpoint;
    cmd.zcl_basic_cmd.src_endpoint = _endpoint;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    cmd.cluster_id = rfidzb::CLUSTER_ID;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.dis_default_resp = 0;
    cmd.custom_cmd_id = rfidzb::CMD_TAG_REPORT;
    cmd.data.type = ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING;  // [len][body]; para strings el SDK deduce el tamaño del prefijo
    cmd.data.size = static_cast<uint16_t>(n);
    cmd.data.value = wire;

    // Espera acotada: nunca bloquear indefinidamente por el lock del stack.
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) return false;
    esp_zb_zcl_custom_cluster_cmd_req(&cmd);
    esp_zb_lock_release();
    return true;
  }

private:
  uint8_t protoVersionAttr_ = rfidzb::PROTO_VERSION;
};
