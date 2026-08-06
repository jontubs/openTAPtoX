#ifndef OPENTAPTOX_MQTT_STATUS_ROWS_H
#define OPENTAPTOX_MQTT_STATUS_ROWS_H

#include <Arduino.h>

#include "models.h"
#include "power_status.h"
#include "utils.h"

template <typename PublishSubScalarFn,
          typename PublishSubScalarU16Fn,
          typename YieldFn>
static void otxPublishPanelMapStatusRows(const UserPanelMapEntry* panelMap,
                                         uint16_t panelFieldCount,
                                         PublishSubScalarFn publishSubScalar,
                                         PublishSubScalarU16Fn publishSubScalarU16,
                                         YieldFn yieldNow) {
  if (panelMap == nullptr) {
    return;
  }
  for (uint16_t i = 0; i < panelFieldCount; ++i) {
    if (panelMap[i].label[0] == '\0' || panelMap[i].longAddr[0] == '\0') {
      continue;
    }
    char prefix[MQTT_TOPIC_LEN];
    snprintf(prefix, sizeof(prefix), "status/panel_map/%s", panelMap[i].label);
    prefix[sizeof(prefix) - 1] = '\0';
    publishSubScalarU16(prefix, "index", (uint16_t)(i + 1U), TIGO_MQTT_RETAIN_STATUS);
    publishSubScalar(prefix, "active", "1", TIGO_MQTT_RETAIN_STATUS);
    publishSubScalar(prefix, "long_addr", panelMap[i].longAddr, TIGO_MQTT_RETAIN_STATUS);
    yieldNow();
  }
}

template <typename LookupPanelLabelFn,
          typename FindPowerByNodeIdFn,
          typename PublishSubScalarFn,
          typename PublishSubScalarU16Fn,
          typename PublishSubScalarU32Fn>
static void otxPublishNodeMapStatusRows(const NodeMapEntry* nodeMap,
                                        size_t nodeMapCapacity,
                                        uint32_t now,
                                        uint32_t sampleFreshMs,
                                        LookupPanelLabelFn lookupPanelLabel,
                                        FindPowerByNodeIdFn findPowerByNodeId,
                                        PublishSubScalarFn publishSubScalar,
                                        PublishSubScalarU16Fn publishSubScalarU16,
                                        PublishSubScalarU32Fn publishSubScalarU32) {
  if (nodeMap == nullptr) {
    return;
  }
  for (size_t i = 0; i < nodeMapCapacity; ++i) {
    // Pending table entries use a high-bit raw ID. They are commissioning
    // state, not an installed optimizer, and never belong in MQTT status.
    if (!nodeMap[i].valid || nodeMap[i].pending || nodeMap[i].nodeId == 0 ||
        nodeMap[i].nodeId >= 0x8000U || nodeMap[i].rawNodeId != nodeMap[i].nodeId) {
      continue;
    }
    char prefix[MQTT_TOPIC_LEN];
    snprintf(prefix, sizeof(prefix), "status/nodes/node_%u", (unsigned)nodeMap[i].nodeId);
    prefix[sizeof(prefix) - 1] = '\0';
    publishSubScalarU16(prefix, "node_id", nodeMap[i].nodeId, TIGO_MQTT_RETAIN_STATUS);
    publishSubScalar(prefix, "long_addr", nodeMap[i].longAddr, TIGO_MQTT_RETAIN_STATUS);
    const char* label = lookupPanelLabel(nodeMap[i].longAddr);
    publishSubScalar(prefix, "panel_label", label ? label : "", TIGO_MQTT_RETAIN_STATUS);
    const PowerReport* power = findPowerByNodeId(nodeMap[i].nodeId);
    publishSubScalar(prefix, "has_power", power ? "1" : "0", TIGO_MQTT_RETAIN_STATUS);
    if (power != nullptr) {
      const uint32_t ageMs = otxSampleAgeMs(*power, now);
      char shortHex[7];
      formatHex4(power->shortAddr, shortHex, sizeof(shortHex));
      publishSubScalar(prefix, "short_addr_hex", shortHex, TIGO_MQTT_RETAIN_STATUS);
      publishSubScalarU32(prefix, "age_ms", ageMs, TIGO_MQTT_RETAIN_STATUS);
      publishSubScalar(prefix, "fresh", otxIsFreshAge(ageMs, sampleFreshMs) ? "1" : "0", TIGO_MQTT_RETAIN_STATUS);
    } else {
      publishSubScalar(prefix, "short_addr_hex", "", TIGO_MQTT_RETAIN_STATUS);
      publishSubScalarU32(prefix, "age_ms", 0, TIGO_MQTT_RETAIN_STATUS);
      publishSubScalar(prefix, "fresh", "0", TIGO_MQTT_RETAIN_STATUS);
    }
  }
}

#endif
