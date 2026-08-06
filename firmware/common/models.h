#ifndef OPENTAPTOX_MODELS_H
#define OPENTAPTOX_MODELS_H

#include <Arduino.h>

// This shared header depends on target-specific limits from config.h.
// Include config.h before including this file.

struct UserPanelMapEntry {
  char label[12];
  char longAddr[17];
};

struct AggregatePowerStatus {
  uint16_t validPower;
  uint16_t freshNodes;
  uint16_t staleNodes;
  uint16_t expiredNodes;
  uint32_t newestSampleAgeMs;
  uint32_t oldestSampleAgeMs;
  uint32_t avgSampleAgeMs;
  float liveSumInputW;
  float heldSumInputW;
};

struct GatewayFrame {
  bool valid;
  bool fromGateway;
  bool crcOk;
  uint16_t gatewayId;
  uint16_t addrRaw;
  uint16_t typeCode;
  uint16_t payloadLen;
  uint8_t payload[MAX_FRAME_PAYLOAD];
};

struct RxStatus {
  uint16_t statusType;
  bool hasPacketCounterHigh;
  uint8_t packetCounterHigh;
  uint8_t packetCounterLow;
  uint16_t slotCounterGateway;
  uint8_t rxBuffersUsed;
  uint8_t txBuffersFree;
  bool hasRxBuffersUsed;
  bool hasTxBuffersFree;
};

struct NodeMapEntry {
  bool valid;
  bool pending;
  bool rfConfirmed;
  uint16_t nodeId;
  uint16_t rawNodeId;
  char longAddr[17];
  uint32_t updatedMs;
};

struct PowerReport {
  bool valid;
  uint16_t nodeId;
  uint16_t shortAddr;
  uint16_t gatewaySlotCounter;
  uint16_t slotCounterReport;
  float vinV;
  float voutV;
  float iinA;
  float tempC;
  float powerInW;
  float dutyPct;
  uint8_t rssi;
  char unknownHex[7];
  char longAddr[17];
  char panelLabel[8];
  uint32_t updatedMs;
};

struct RecentEvent {
  uint32_t ms;
  char text[EVENT_TEXT_LEN];
};

struct PersistedState {
  uint32_t magic;
  uint16_t version;
  uint16_t gatewayId;
  uint16_t nextPacketNumber;
  char gatewayLongAddr[17];
  uint16_t panelFieldCount;
  uint8_t panelMapPresent[(TIGO_MAX_OPTIMIZERS + 7U) / 8U];
  uint8_t panelMapLongAddr[TIGO_MAX_OPTIMIZERS][8];
  uint32_t checksum;
};

#endif
