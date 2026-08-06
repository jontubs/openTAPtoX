#ifndef OPENTAPTOX_MQTT_RAW_FRAME_H
#define OPENTAPTOX_MQTT_RAW_FRAME_H

#include <Arduino.h>
#include <PubSubClient.h>

#include "models.h"
#include "mqtt_publish.h"
#include "utils.h"

// This shared header depends on target-specific limits from config.h.
// Include config.h before including this file.

static void otxAppendByteHex(String& out, uint8_t value) {
  static const char hex[] = "0123456789ABCDEF";
  out += hex[(value >> 4) & 0x0F];
  out += hex[value & 0x0F];
}

static void otxBuildRawFrameJsonBodyFields(uint16_t gatewayId,
                                           uint16_t addrRaw,
                                           uint16_t typeCode,
                                           bool fromGateway,
                                           bool crcOk,
                                           uint32_t deviceMs,
                                           const uint8_t* payload,
                                           size_t payloadLen,
                                           String& body) {
  char gatewayHex[7];
  char addrHex[7];
  char typeHex[7];
  formatHex4(gatewayId, gatewayHex, sizeof(gatewayHex));
  formatHex4(addrRaw, addrHex, sizeof(addrHex));
  formatHex4(typeCode, typeHex, sizeof(typeHex));

  body = "";
  body.reserve(104 + payloadLen * 2U);
  body += "{";
  body += "\"gateway_id_hex\":\""; body += gatewayHex; body += "\",";
  body += "\"addr_raw_hex\":\""; body += addrHex; body += "\",";
  body += "\"type_code_hex\":\""; body += typeHex; body += "\",";
  body += "\"from_gateway\":"; body += (fromGateway ? "true" : "false"); body += ",";
  body += "\"crc_ok\":"; body += (crcOk ? "true" : "false"); body += ",";
  body += "\"device_ms\":"; body += String(deviceMs); body += ",";
  body += "\"payload_hex\":\"";
  for (size_t i = 0; i < payloadLen; ++i) {
    otxAppendByteHex(body, payload[i]);
  }
  body += "\"";
  body += "}";
}

static void otxBuildRawFrameJsonBody(const GatewayFrame& frame,
                                     uint32_t deviceMs,
                                     String& body) {
  otxBuildRawFrameJsonBodyFields(frame.gatewayId,
                                 frame.addrRaw,
                                 frame.typeCode,
                                 frame.fromGateway,
                                 frame.crcOk,
                                 deviceMs,
                                 frame.payload,
                                 frame.payloadLen,
                                 body);
}

static bool otxPublishRawFrame(PubSubClient& client,
                               const char* baseTopic,
                               const GatewayFrame& frame,
                               uint32_t deviceMs) {
  if (!client.connected()) {
    return false;
  }
  String body;
  otxBuildRawFrameJsonBody(frame, deviceMs, body);
  return otxMqttPublish(client, baseTopic, "raw/frame", body, false);
}

static bool otxPublishRawFrameFields(PubSubClient& client,
                                     const char* baseTopic,
                                     uint16_t gatewayId,
                                     uint16_t addrRaw,
                                     uint16_t typeCode,
                                     bool fromGateway,
                                     bool crcOk,
                                     uint32_t deviceMs,
                                     const uint8_t* payload,
                                     size_t payloadLen) {
  if (!client.connected()) {
    return false;
  }
  String body;
  otxBuildRawFrameJsonBodyFields(gatewayId, addrRaw, typeCode,
                                 fromGateway, crcOk, deviceMs,
                                 payload, payloadLen, body);
  return otxMqttPublish(client, baseTopic, "raw/frame", body, false);
}

#endif
