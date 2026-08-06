#ifndef OPENTAPTOX_POWER_STATUS_H
#define OPENTAPTOX_POWER_STATUS_H

#include <Arduino.h>

#include "models.h"

static uint32_t otxSampleAgeMs(const PowerReport& slot, uint32_t now) {
  return slot.valid ? (uint32_t)(now - slot.updatedMs) : 0UL;
}

static bool otxIsFreshAge(uint32_t ageMs, uint32_t freshMs) {
  return ageMs <= freshMs;
}

static float otxHeldWeightForAge(uint32_t ageMs, uint32_t freshMs, uint32_t holdMs) {
  if (ageMs <= freshMs) {
    return 1.0f;
  }
  if (ageMs >= holdMs) {
    return 0.0f;
  }
  const float span = (float)(holdMs - freshMs);
  const float remaining = (float)(holdMs - ageMs);
  return remaining / span;
}

static AggregatePowerStatus otxBuildAggregatePowerStatus(const PowerReport* slots,
                                                         size_t slotCount,
                                                         uint32_t now,
                                                         uint32_t freshMs,
                                                         uint32_t holdMs) {
  AggregatePowerStatus out{};
  uint64_t ageSum = 0;
  bool firstAge = true;

  if (slots == nullptr) {
    return out;
  }

  for (size_t i = 0; i < slotCount; ++i) {
    if (!slots[i].valid) {
      continue;
    }
    ++out.validPower;
    const uint32_t ageMs = otxSampleAgeMs(slots[i], now);
    const bool fresh = otxIsFreshAge(ageMs, freshMs);
    const float holdWeight = otxHeldWeightForAge(ageMs, freshMs, holdMs);

    if (fresh) {
      ++out.freshNodes;
      out.liveSumInputW += slots[i].powerInW;
    } else if (holdWeight > 0.0f) {
      ++out.staleNodes;
    } else {
      ++out.expiredNodes;
    }

    if (holdWeight > 0.0f) {
      out.heldSumInputW += slots[i].powerInW * holdWeight;
    }

    if (firstAge) {
      out.newestSampleAgeMs = ageMs;
      out.oldestSampleAgeMs = ageMs;
      firstAge = false;
    } else {
      if (ageMs < out.newestSampleAgeMs) {
        out.newestSampleAgeMs = ageMs;
      }
      if (ageMs > out.oldestSampleAgeMs) {
        out.oldestSampleAgeMs = ageMs;
      }
    }
    ageSum += ageMs;
  }

  if (out.validPower > 0) {
    out.avgSampleAgeMs = (uint32_t)(ageSum / out.validPower);
  }
  return out;
}

#endif
