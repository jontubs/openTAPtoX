#ifndef OPENTAPTOX_PERSISTENCE_CODEC_H
#define OPENTAPTOX_PERSISTENCE_CODEC_H

#include <Arduino.h>
#include <string.h>

#include "models.h"
#include "platform_runtime.h"
#include "utils.h"

// This shared header depends on target-specific limits from config.h.
// Include config.h before including this file.

enum class PersistentStateLoadStatus : uint8_t {
  Missing,
  Invalid,
  Loaded,
};

static void persistentStateBytesToHex(const uint8_t* data, size_t len, char* out, size_t outLen) {
  if (outLen == 0) {
    return;
  }
  size_t pos = 0;
  for (size_t i = 0; i < len && (pos + 2) < outLen; ++i) {
    pos += (size_t)snprintf(out + pos, outLen - pos, "%02X", data[i]);
  }
  out[(pos < outLen) ? pos : (outLen - 1)] = '\0';
}

static uint16_t persistentStateClampPanelFieldCount(uint16_t panelFieldCount, size_t panelMapCapacity) {
  uint16_t clamped = clampPanelFieldCountValue(panelFieldCount);
  if ((size_t)clamped > panelMapCapacity) {
    clamped = (uint16_t)panelMapCapacity;
  }
  return clamped;
}

static void persistentStateInitializeDefaultPanelMap(UserPanelMapEntry* panelMap, size_t panelMapCapacity) {
  if (panelMap == nullptr) {
    return;
  }
  for (size_t i = 0; i < panelMapCapacity; ++i) {
    makeDefaultPanelLabel(i, panelMap[i].label, sizeof(panelMap[i].label));
    panelMap[i].longAddr[0] = '\0';
  }
}

static void persistentStateResetControllerState(uint16_t defaultGatewayId,
                                                uint16_t initialPacketNumber,
                                                uint16_t defaultPanelFieldCount,
                                                uint16_t* gatewayId,
                                                uint16_t* nextPacketNumber,
                                                char* gatewayLongAddr,
                                                size_t gatewayLongAddrLen,
                                                uint16_t* panelFieldCount) {
  if (gatewayId != nullptr) {
    *gatewayId = defaultGatewayId;
  }
  if (nextPacketNumber != nullptr) {
    *nextPacketNumber = initialPacketNumber;
  }
  if (gatewayLongAddr != nullptr && gatewayLongAddrLen > 0) {
    gatewayLongAddr[0] = '\0';
  }
  if (panelFieldCount != nullptr) {
    *panelFieldCount = defaultPanelFieldCount;
  }
}

static uint32_t persistentStateChecksum(const PersistedState& st) {
  PersistedState tmp = st;
  tmp.checksum = 0;
  return fnv1a32(reinterpret_cast<const uint8_t*>(&tmp), sizeof(tmp));
}

static void persistentStateEncode(
    PersistedState* st,
    uint16_t gatewayId,
    uint16_t nextPacketNumber,
    const char* gatewayLongAddr,
    uint16_t panelFieldCount,
    const UserPanelMapEntry* panelMap,
    size_t panelMapCapacity) {
  if (st == nullptr) {
    return;
  }

  memset(st, 0, sizeof(*st));
  st->magic = TIGO_STATE_MAGIC;
  st->version = TIGO_STATE_VERSION;
  st->gatewayId = gatewayId;
  st->nextPacketNumber = nextPacketNumber;
  copyString(st->gatewayLongAddr, sizeof(st->gatewayLongAddr), gatewayLongAddr);
  st->panelFieldCount = persistentStateClampPanelFieldCount(panelFieldCount, panelMapCapacity);

  if (panelMap != nullptr) {
    for (uint16_t i = 0; i < st->panelFieldCount; ++i) {
      uint8_t bin[8];
      if (hex16ToBytes(panelMap[i].longAddr, bin)) {
        bitArraySet(st->panelMapPresent, i, true);
        memcpy(st->panelMapLongAddr[i], bin, sizeof(bin));
      }
    }
  }

  st->checksum = persistentStateChecksum(*st);
}

static bool persistentStateDecode(
    const PersistedState& st,
    uint16_t* gatewayId,
    uint16_t* nextPacketNumber,
    char* gatewayLongAddr,
    size_t gatewayLongAddrLen,
    uint16_t* panelFieldCount,
    UserPanelMapEntry* panelMap,
    size_t panelMapCapacity) {
  if (st.magic != TIGO_STATE_MAGIC || st.version != TIGO_STATE_VERSION ||
      st.checksum != persistentStateChecksum(st)) {
    return false;
  }

  if (gatewayId != nullptr) {
    *gatewayId = st.gatewayId;
  }
  if (nextPacketNumber != nullptr) {
    *nextPacketNumber = st.nextPacketNumber;
  }
  if (gatewayLongAddr != nullptr) {
    copyString(gatewayLongAddr, gatewayLongAddrLen, st.gatewayLongAddr);
  }

  const uint16_t fieldCount = persistentStateClampPanelFieldCount(st.panelFieldCount, panelMapCapacity);
  if (panelFieldCount != nullptr) {
    *panelFieldCount = fieldCount;
  }

  if (panelMap != nullptr) {
    for (uint16_t i = 0; i < fieldCount; ++i) {
      if (!bitArrayGet(st.panelMapPresent, i)) {
        continue;
      }
      persistentStateBytesToHex(st.panelMapLongAddr[i], 8, panelMap[i].longAddr, sizeof(panelMap[i].longAddr));
    }
  }

  return true;
}

static PersistentStateLoadStatus persistentStateLoadFromStore(
    PlatformPersistentStore& store,
    const char* key,
    uint16_t* gatewayId,
    uint16_t* nextPacketNumber,
    char* gatewayLongAddr,
    size_t gatewayLongAddrLen,
    uint16_t* panelFieldCount,
    UserPanelMapEntry* panelMap,
    size_t panelMapCapacity) {
  PersistedState st{};
  const size_t bytesRead = store.loadBytes(key, &st, sizeof(st));
  if (bytesRead != sizeof(st)) {
    return PersistentStateLoadStatus::Missing;
  }
  if (!persistentStateDecode(st, gatewayId, nextPacketNumber, gatewayLongAddr, gatewayLongAddrLen,
                             panelFieldCount, panelMap, panelMapCapacity)) {
    return PersistentStateLoadStatus::Invalid;
  }
  return PersistentStateLoadStatus::Loaded;
}

static PersistentStateLoadStatus persistentStateLoadOrResetControllerState(
    PlatformPersistentStore& store,
    const char* key,
    uint16_t defaultGatewayId,
    uint16_t initialPacketNumber,
    uint16_t defaultPanelFieldCount,
    uint16_t* gatewayId,
    uint16_t* nextPacketNumber,
    char* gatewayLongAddr,
    size_t gatewayLongAddrLen,
    uint16_t* panelFieldCount,
    UserPanelMapEntry* panelMap,
    size_t panelMapCapacity) {
  persistentStateInitializeDefaultPanelMap(panelMap, panelMapCapacity);

  const PersistentStateLoadStatus status = persistentStateLoadFromStore(
      store,
      key,
      gatewayId,
      nextPacketNumber,
      gatewayLongAddr,
      gatewayLongAddrLen,
      panelFieldCount,
      panelMap,
      panelMapCapacity);
  if (status != PersistentStateLoadStatus::Loaded) {
    persistentStateResetControllerState(defaultGatewayId,
                                        initialPacketNumber,
                                        defaultPanelFieldCount,
                                        gatewayId,
                                        nextPacketNumber,
                                        gatewayLongAddr,
                                        gatewayLongAddrLen,
                                        panelFieldCount);
  }
  return status;
}

static bool persistentStateSaveToStore(
    PlatformPersistentStore& store,
    const char* key,
    uint16_t gatewayId,
    uint16_t nextPacketNumber,
    const char* gatewayLongAddr,
    uint16_t panelFieldCount,
    const UserPanelMapEntry* panelMap,
    size_t panelMapCapacity) {
  PersistedState st;
  persistentStateEncode(&st, gatewayId, nextPacketNumber, gatewayLongAddr,
                        panelFieldCount, panelMap, panelMapCapacity);
  return store.saveBytes(key, &st, sizeof(st));
}

#endif
