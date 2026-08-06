#ifndef OPENTAPTOX_MQTT_TELEMETRY_H
#define OPENTAPTOX_MQTT_TELEMETRY_H

#include <Arduino.h>

#include "models.h"
#include "power_status.h"
#include "utils.h"

struct OtxTelemetryPublishValues {
  uint32_t ageMs;
  bool fresh;
};

static OtxTelemetryPublishValues otxBuildTelemetryPublishValues(const PowerReport& slot,
                                                                uint32_t now,
                                                                uint32_t freshMs) {
  const uint32_t ageMs = otxSampleAgeMs(slot, now);
  const bool fresh = otxIsFreshAge(ageMs, freshMs);
  return OtxTelemetryPublishValues{
      ageMs,
      fresh,
  };
}

template <typename PublishSubScalarFn,
          typename PublishSubScalarU32Fn,
          typename PublishSubScalarFloatFn>
static void otxPublishTelemetryValues(const char* suffix,
                                     const PowerReport& slot,
                                     const OtxTelemetryPublishValues& values,
                                     PublishSubScalarFn publishSubScalar,
                                     PublishSubScalarU32Fn publishSubScalarU32,
                                     PublishSubScalarFloatFn publishSubScalarFloat) {
  publishSubScalarFloat(suffix, "vin_v", slot.vinV, 3, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalarFloat(suffix, "vout_v", slot.voutV, 3, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalarFloat(suffix, "iin_a", slot.iinA, 3, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalarFloat(suffix, "power", slot.powerInW, 3, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalarFloat(suffix, "temp_c", slot.tempC, 1, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalarU32(suffix, "rssi", slot.rssi, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalarU32(suffix, "age_ms", values.ageMs, TIGO_MQTT_RETAIN_TELEMETRY);
  publishSubScalar(suffix, "fresh", values.fresh ? "1" : "0", TIGO_MQTT_RETAIN_TELEMETRY);
}

template <typename PublishTelemetryFn, typename YieldFn>
static void otxPublishAllTelemetry(const PowerReport* powerSlots,
                                   size_t slotCount,
                                   PublishTelemetryFn publishTelemetry,
                                   YieldFn yieldNow) {
  if (powerSlots == nullptr) {
    return;
  }
  for (size_t i = 0; i < slotCount; ++i) {
    if (!powerSlots[i].valid) {
      continue;
    }
    publishTelemetry(powerSlots[i]);
    yieldNow();
  }
}

#endif
