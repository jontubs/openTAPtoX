#ifndef OPENTAPTOX_MQTT_PERIODIC_H
#define OPENTAPTOX_MQTT_PERIODIC_H

#include <Arduino.h>

template <typename PublishScalarFn, typename PublishScalarU32Fn>
static void otxPublishMqttHeartbeat(PublishScalarFn publishScalar,
                                    PublishScalarU32Fn publishScalarU32,
                                    uint32_t now) {
  publishScalar("status/esp/heartbeat", "1", false);
  publishScalarU32("status/esp/heartbeat_ms", now, false);
  publishScalarU32("status/esp/heartbeat_s", now / 1000UL, false);
}

template <typename DedupeFn,
          typename ClearLegacyFn,
          typename PublishDiscoveryFn,
          typename PublishHeartbeatFn,
          typename PublishStatusFn,
          typename PublishTelemetryFn>
static void otxRunMqttPeriodicCycle(bool mqttConnected,
                                    uint32_t now,
                                    uint32_t heartbeatEveryMs,
                                    uint32_t statusEveryMs,
                                    uint32_t telemetryEveryMs,
                                    uint32_t* lastHeartbeatPublishMs,
                                    uint32_t* lastStatusPublishMs,
                                    uint32_t* lastTelemetryPublishMs,
                                    bool* statusDirty,
                                    DedupeFn dedupePowerSlots,
                                    ClearLegacyFn clearLegacyStateTopics,
                                    PublishDiscoveryFn publishLegacyDiscovery,
                                    PublishHeartbeatFn publishHeartbeat,
                                    PublishStatusFn publishStatus,
                                    PublishTelemetryFn publishTelemetry) {
  if (!mqttConnected) {
    return;
  }

  dedupePowerSlots();
  clearLegacyStateTopics();
  publishLegacyDiscovery(now);

  if (lastHeartbeatPublishMs != nullptr &&
      (now - *lastHeartbeatPublishMs) >= heartbeatEveryMs) {
    publishHeartbeat(now);
    *lastHeartbeatPublishMs = now;
  }
  if (statusDirty != nullptr &&
      lastStatusPublishMs != nullptr &&
      (*statusDirty || (now - *lastStatusPublishMs) >= statusEveryMs)) {
    publishStatus();
    *lastStatusPublishMs = now;
    *statusDirty = false;
  }
  if (lastTelemetryPublishMs != nullptr &&
      (now - *lastTelemetryPublishMs) >= telemetryEveryMs) {
    publishTelemetry();
    *lastTelemetryPublishMs = now;
  }
}

#endif
