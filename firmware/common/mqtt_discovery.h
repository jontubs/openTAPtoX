#ifndef OPENTAPTOX_MQTT_DISCOVERY_H
#define OPENTAPTOX_MQTT_DISCOVERY_H

#include <Arduino.h>

#include "models.h"
#include "utils.h"

static void otxJoinTopic(const char* prefix, const char* key, char* out, size_t outLen) {
  if (outLen == 0) {
    return;
  }
  if (prefix == nullptr || prefix[0] == '\0') {
    copyString(out, outLen, key);
    return;
  }
  const size_t prefixLen = strlen(prefix);
  if (prefix[prefixLen - 1] == '/') {
    snprintf(out, outLen, "%s%s", prefix, key);
  } else {
    snprintf(out, outLen, "%s/%s", prefix, key);
  }
  out[outLen - 1] = '\0';
}

static void otxMakeMqttTopic(const char* baseTopic, const char* suffix, char* out, size_t outLen) {
  if (outLen == 0) {
    return;
  }
  if (baseTopic == nullptr || baseTopic[0] == '\0') {
    out[0] = '\0';
    return;
  }
  const size_t baseLen = strlen(baseTopic);
  if (baseTopic[baseLen - 1] == '/') {
    snprintf(out, outLen, "%s%s", baseTopic, suffix);
  } else {
    snprintf(out, outLen, "%s/%s", baseTopic, suffix);
  }
  out[outLen - 1] = '\0';
}

static void otxMakeLegacyNodeDeviceId(uint16_t nodeId, char* out, size_t outLen) {
  snprintf(out, outLen, "node_%u", (unsigned)nodeId);
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static void otxMakeLegacyLabelPrefix(const char* legacyBaseTopic, const char* label, char* out, size_t outLen) {
  snprintf(out, outLen, "%s/%s", legacyBaseTopic, label ? label : "");
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static String otxMakePanelDisplayNameString(const char* label) {
  char tmp[32];
  makePanelDisplayName(label, tmp, sizeof(tmp));
  return String(tmp);
}

static void otxMakeLegacyDisplayPrefix(const char* legacyBaseTopic, const char* label, char* out, size_t outLen) {
  const String display = otxMakePanelDisplayNameString(label);
  snprintf(out, outLen, "%s/%s", legacyBaseTopic, display.c_str());
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static void otxMakeLegacyNodePrefix(const char* legacyBaseTopic, uint16_t nodeId, char* out, size_t outLen) {
  char deviceId[20];
  otxMakeLegacyNodeDeviceId(nodeId, deviceId, sizeof(deviceId));
  snprintf(out, outLen, "%s/%s", legacyBaseTopic, deviceId);
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static void otxMakeTelemetryPrimarySuffix(const PowerReport& slot, char* out, size_t outLen) {
  if (slot.panelLabel[0]) {
    snprintf(out, outLen, "telemetry/%s", slot.panelLabel);
  } else {
    snprintf(out, outLen, "telemetry/node_%u", (unsigned)slot.nodeId);
  }
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static void otxMakeDiscoveryDeviceName(const PowerReport& slot, char* out, size_t outLen) {
  if (slot.panelLabel[0]) {
    makePanelDisplayName(slot.panelLabel, out, outLen);
    return;
  }
  snprintf(out, outLen, "Tigo Node %u", (unsigned)slot.nodeId);
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static void otxMakeDiscoveryObjectBase(const PowerReport& slot, char* out, size_t outLen) {
  char deviceName[48];
  char slug[48];
  otxMakeDiscoveryDeviceName(slot, deviceName, sizeof(deviceName));
  slugifyIdentifierToBuffer(deviceName, slug, sizeof(slug));
  snprintf(out, outLen, "opentaptox_%s", slug);
  if (outLen > 0) {
    out[outLen - 1] = '\0';
  }
}

static String otxMakeDiscoveryDeviceNameString(const PowerReport& slot) {
  char tmp[48];
  otxMakeDiscoveryDeviceName(slot, tmp, sizeof(tmp));
  return String(tmp);
}

static String otxMakeDiscoveryObjectBaseString(const PowerReport& slot) {
  char tmp[48];
  otxMakeDiscoveryObjectBase(slot, tmp, sizeof(tmp));
  return String(tmp);
}

static String otxMakeLegacyNodeDeviceIdString(uint16_t nodeId) {
  char tmp[20];
  otxMakeLegacyNodeDeviceId(nodeId, tmp, sizeof(tmp));
  return String(tmp);
}

static String otxMakeLegacyLabelPrefixString(const char* legacyBaseTopic, const char* label) {
  char tmp[MQTT_TOPIC_LEN];
  otxMakeLegacyLabelPrefix(legacyBaseTopic, label, tmp, sizeof(tmp));
  return String(tmp);
}

static String otxMakeLegacyDisplayPrefixString(const char* legacyBaseTopic, const char* label) {
  char tmp[MQTT_TOPIC_LEN];
  otxMakeLegacyDisplayPrefix(legacyBaseTopic, label, tmp, sizeof(tmp));
  return String(tmp);
}

static String otxMakeLegacyNodePrefixString(const char* legacyBaseTopic, uint16_t nodeId) {
  char tmp[MQTT_TOPIC_LEN];
  otxMakeLegacyNodePrefix(legacyBaseTopic, nodeId, tmp, sizeof(tmp));
  return String(tmp);
}

static String otxMakeHomeAssistantSensorConfigTopic(const char* discoveryPrefix, const String& objectBase, const char* key) {
  return String(discoveryPrefix) + "/sensor/" + objectBase + "_" + key + "/config";
}

static String otxMakeLegacyDeviceConfigTopic(const char* discoveryPrefix, const char* label) {
  String labelLower = String(label ? label : "");
  labelLower.toLowerCase();
  return String(discoveryPrefix) + "/device/tigo_" + labelLower + "/config";
}

static String otxMakeLegacyNodeConfigTopic(const char* discoveryPrefix, uint16_t nodeId) {
  return String(discoveryPrefix) + "/device/tigo_node_" + String(nodeId) + "/config";
}

static String otxBuildOptimizerSensorDiscoveryJson(const String& sensorName,
                                                   const String& objectId,
                                                   const String& stateTopic,
                                                   const String& objectBase,
                                                   const String& deviceName,
                                                   const char* unit,
                                                   const char* deviceClass,
                                                   const char* entityCategory,
                                                   bool measurement,
                                                   const char* firmwareVersion,
                                                   const char* serialNumber) {
  String body;
  body.reserve(1024);
  body += "{";
  body += "\"name\":\""; body += jsonEscape(sensorName); body += "\",";
  body += "\"unique_id\":\""; body += objectId; body += "\",";
  body += "\"state_topic\":\""; body += jsonEscape(stateTopic); body += "\",";
  body += "\"object_id\":\""; body += objectId; body += "\",";
  if (unit != nullptr && unit[0] != '\0') {
    body += "\"unit_of_measurement\":\""; body += unit; body += "\"";
    if (deviceClass != nullptr || entityCategory != nullptr || measurement) {
      body += ",";
    }
  }
  if (deviceClass != nullptr) {
    body += "\"device_class\":\""; body += deviceClass; body += "\"";
    if (entityCategory != nullptr || measurement) {
      body += ",";
    }
  }
  if (measurement) {
    body += "\"state_class\":\"measurement\"";
    if (entityCategory != nullptr) {
      body += ",";
    }
  }
  if (entityCategory != nullptr) {
    body += "\"entity_category\":\""; body += entityCategory; body += "\"";
  }
  body += ",";
  body += "\"device\":{";
  body += "\"identifiers\":[\""; body += objectBase; body += "\"],";
  body += "\"name\":\""; body += jsonEscape(deviceName); body += "\",";
  body += "\"manufacturer\":\"Tigo\",";
  body += "\"model\":\"Optimizer\",";
  body += "\"sw_version\":\""; body += firmwareVersion; body += "\"";
  if (serialNumber != nullptr && serialNumber[0] != '\0') {
    body += ",\"serial_number\":\""; body += jsonEscape(serialNumber); body += "\"";
  }
  body += "},";
  body += "\"origin\":{";
  body += "\"name\":\"openTAPtoX\",";
  body += "\"sw_version\":\""; body += firmwareVersion; body += "\"";
  body += "}";
  body += "}";
  return body;
}

static String otxBuildSummarySensorDiscoveryJson(const String& objectBase,
                                                 const String& stateTopic,
                                                 const char* key,
                                                 const char* name,
                                                 const char* unit,
                                                 const char* deviceClass,
                                                 bool measurement,
                                                 const char* firmwareVersion) {
  String body;
  body.reserve(768);
  body += "{";
  body += "\"name\":\""; body += name; body += "\",";
  body += "\"unique_id\":\""; body += objectBase; body += "_"; body += key; body += "\",";
  body += "\"state_topic\":\""; body += jsonEscape(stateTopic); body += "\",";
  body += "\"object_id\":\""; body += objectBase; body += "_"; body += key; body += "\",";
  if (unit != nullptr && unit[0] != '\0') {
    body += "\"unit_of_measurement\":\""; body += unit; body += "\",";
  }
  if (deviceClass != nullptr && deviceClass[0] != '\0') {
    body += "\"device_class\":\""; body += deviceClass; body += "\",";
  }
  if (measurement) {
    body += "\"state_class\":\"measurement\",";
  }
  body += "\"device\":{";
  body += "\"identifiers\":[\""; body += objectBase; body += "\"],";
  body += "\"name\":\"Tigo Gesamtanlage\",";
  body += "\"manufacturer\":\"Tigo\",";
  body += "\"model\":\"Summary\",";
  body += "\"sw_version\":\""; body += firmwareVersion; body += "\"";
  body += "},";
  body += "\"origin\":{";
  body += "\"name\":\"openTAPtoX\",";
  body += "\"sw_version\":\""; body += firmwareVersion; body += "\"";
  body += "}";
  body += "}";
  return body;
}

#endif
