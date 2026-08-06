#ifndef OPENTAPTOX_MQTT_STATUS_PUBLISH_H
#define OPENTAPTOX_MQTT_STATUS_PUBLISH_H

#include <Arduino.h>

#include "models.h"

struct OtxMqttStatusSnapshot {
  const char* projectTitle;
  const char* hostname;
  const char* ip;
  const char* wifiMode;
  const char* gatewayLongAddr;
  const char* resetReason;
  const char* firmwareVersion;
  const char* versionText;
  bool wifiConnected;
  bool apMode;
  bool mqttConnected;
  bool pollingEnabled;
  uint16_t gatewayId;
  uint16_t nextPacketNumber;
  uint16_t lastRequestedPacketNumber;
  uint16_t nodeCount;
  uint16_t powerCount;
  uint16_t panelFieldCount;
  uint16_t maxOptimizers;
  uint32_t framesRx;
  uint32_t framesCrcError;
  uint32_t pollsSent;
  uint32_t pollTimeouts;
  uint32_t pollIntervalMs;
  uint32_t pollTimeoutMs;
  uint32_t uptimeMs;
  uint32_t freeHeap;
  uint32_t sampleFreshMs;
  uint32_t sampleHoldMs;
  AggregatePowerStatus aggregatePower;
};

template <typename PublishScalarFn,
          typename PublishScalarU32Fn,
          typename PublishScalarU16Fn,
          typename PublishScalarFloatFn,
          typename PublishScalarHex4Fn,
          typename PublishPanelMapFn,
          typename PublishNodeMapFn>
static void otxPublishMqttStatusSnapshot(const OtxMqttStatusSnapshot& snapshot,
                                         PublishScalarFn publishScalar,
                                         PublishScalarU32Fn publishScalarU32,
                                         PublishScalarU16Fn publishScalarU16,
                                         PublishScalarFloatFn publishScalarFloat,
                                         PublishScalarHex4Fn publishScalarHex4,
                                         PublishPanelMapFn publishPanelMap,
                                         PublishNodeMapFn publishNodeMap) {
  publishScalar("status/project/title", snapshot.projectTitle, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/hostname", snapshot.hostname, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/ip", snapshot.ip, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/wifi_mode", snapshot.wifiMode, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/wifi_connected", snapshot.wifiConnected ? "1" : "0", TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/ap_mode", snapshot.apMode ? "1" : "0", TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/mqtt_connected", snapshot.mqttConnected ? "1" : "0", TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/polling_enabled", snapshot.pollingEnabled ? "1" : "0", TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/esp/uptime_ms", snapshot.uptimeMs, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/esp/free_heap", snapshot.freeHeap, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/reset_reason", snapshot.resetReason, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/esp/firmware_version", snapshot.firmwareVersion, TIGO_MQTT_RETAIN_STATUS);

  publishScalarHex4("status/tap/gateway_id_hex", snapshot.gatewayId, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/tap/gateway_long_addr", snapshot.gatewayLongAddr, TIGO_MQTT_RETAIN_STATUS);
  publishScalar("status/tap/version_text", snapshot.versionText, TIGO_MQTT_RETAIN_STATUS);
  publishScalarHex4("status/tap/next_packet_hex", snapshot.nextPacketNumber, TIGO_MQTT_RETAIN_STATUS);
  publishScalarHex4("status/tap/last_requested_packet_hex", snapshot.lastRequestedPacketNumber, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/tap/frames_rx", snapshot.framesRx, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/tap/frames_crc_error", snapshot.framesCrcError, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/tap/polls_sent", snapshot.pollsSent, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/tap/poll_timeouts", snapshot.pollTimeouts, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/tap/poll_interval_ms", snapshot.pollIntervalMs, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/tap/poll_timeout_ms", snapshot.pollTimeoutMs, TIGO_MQTT_RETAIN_STATUS);

  publishScalarU16("status/plant/detected_node_count", snapshot.nodeCount, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU16("status/plant/power_node_count", snapshot.powerCount, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU16("status/plant/configured_panel_count", snapshot.panelFieldCount, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU16("status/plant/max_optimizers", snapshot.maxOptimizers, TIGO_MQTT_RETAIN_STATUS);

  publishPanelMap();
  publishNodeMap();

  publishScalarFloat("status/power/live_sum_input_w", snapshot.aggregatePower.liveSumInputW, 3, TIGO_MQTT_RETAIN_STATUS);
  publishScalarFloat("status/power/held_sum_input_w", snapshot.aggregatePower.heldSumInputW, 3, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU16("status/power/fresh_nodes", snapshot.aggregatePower.freshNodes, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU16("status/power/stale_nodes", snapshot.aggregatePower.staleNodes, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU16("status/power/expired_nodes", snapshot.aggregatePower.expiredNodes, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/power/newest_sample_age_ms", snapshot.aggregatePower.newestSampleAgeMs, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/power/oldest_sample_age_ms", snapshot.aggregatePower.oldestSampleAgeMs, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/power/avg_sample_age_ms", snapshot.aggregatePower.avgSampleAgeMs, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/power/sample_fresh_ms", snapshot.sampleFreshMs, TIGO_MQTT_RETAIN_STATUS);
  publishScalarU32("status/power/sample_hold_ms", snapshot.sampleHoldMs, TIGO_MQTT_RETAIN_STATUS);
}

#endif
