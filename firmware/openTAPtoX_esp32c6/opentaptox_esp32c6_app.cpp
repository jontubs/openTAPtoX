#include <Arduino.h>
#include "opentaptox_esp32c6_app.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <esp_system.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include "config.h"
#include "platform_runtime.h"
#include <otx_common_models.h>
#include <otx_common_mqtt_discovery.h>
#include <otx_common_mqtt_periodic.h>
#include <otx_common_mqtt_publish.h>
#include <otx_common_mqtt_raw_frame.h>
#include <otx_common_mqtt_runtime.h>
#include <otx_common_mqtt_status_publish.h>
#include <otx_common_mqtt_status_rows.h>
#include <otx_common_mqtt_telemetry.h>
#include <otx_common_persistence_codec.h>
#include <otx_common_power_status.h>
#include <otx_common_protocol.h>
#include <otx_common_utils.h>
#include <otx_common_web_ui.h>

class TigoTapMaster;
static TigoTapMaster* g_tigoMqttCallbackTarget = nullptr;
static void handleTigoMqttCallback(char* topic, uint8_t* payload, unsigned int length);

// Refactor pause point:
// Keep the TAP command enums/state local to this controller app for now.
// A first extraction attempt correlated with multi-second receive gaps on live
// hardware after flashing. Continue with only tiny, passive extractions first,
// and re-flash/re-verify on hardware after every step before touching the
// app/runtime/RS485 path again.
enum class TapCommandKind : uint8_t {
  None,
  Ping,
  RsdControl,
  Version,
  NodeTable,
  NetworkStatus,
  RadioConfig,
  PvConfig,
  PvSubcommand,
  Enumerate,
  SimpleFrame,
  BootReceiveSeed,
};

enum class TapCommandPhase : uint8_t {
  Idle,
  WaitingFrame,
  EnumerateStartBurst,
  EnumerateWaitInfo,
  EnumerateWaitAssign,
  EnumerateWaitVerify,
  AddressWaitVerify,
};

enum class NodeSeedState : uint8_t {
  Idle,
  ClearTable,
  SendChunk,
  VerifyNodeTable,
  StartLearn,
  EnablePv,
  Done,
  Failed,
};

enum class RfNodePromotionState : uint8_t {
  Idle,
  CancelWaiting,
  WriteReady,
  WriteWaiting,
  VerifyReady,
  VerifyWaiting,
  StatusReady,
  StatusWaiting,
};

enum class TapBootPath : uint8_t {
  Unknown,
  PassiveListen,
  ReadOnlyWarmAttach,
  ReadOnlyPolling,
  TargetedRecovery,
  LegacyCcaReplay,
};

enum class TapObservedState : uint8_t {
  UnknownUnreachable,
  FactoryOrUnaddressed,
  AddressedRadioUnknown,
  WarmOperational,
  NodeTableEmpty,
  NodeTablePending,
  PartiallyConfirmed,
  FullyConfirmedNoTelemetry,
  TelemetryActiveRsdLocked,
  FullyReleasedStable,
};

enum class WarmAttachPhase : uint8_t {
  Idle,
  PassiveListen,
  ProbeCandidate,
  FactoryAssign,
  FactoryVerify,
  Version,
  RadioSelector0,
  RadioSelector1,
  NetworkStatus,
  NodeTable,
  CursorBootstrapZero,
  CursorBootstrapEeee,
  Complete,
  Failed,
};

enum class BootCursorStrategy : uint8_t {
  Persisted = 0,
  Zero = 1,
  CcaBootstrap = 2,
};

enum class AddressTransactionState : uint8_t {
  None,
  Requested,
  Acknowledged,
  Verified,
  Failed,
};

enum class TraceReplayAction : uint8_t {
  SimpleFrame,
  ReceiveBootstrap,
  PvSubcommand,
};

enum class TraceReplayOutcome : uint8_t {
  FrameResponse,
  NoResponse,
  TapAck,
  RfResponse,
  LocalTapAck,
};

enum class TraceReplayRisk : uint8_t {
  ReadOnly,
  ActiveRf,
  StateChanging,
  Unknown,
};

enum class TraceReplayState : uint8_t {
  Idle,
  Loaded,
  Running,
  WaitingCommand,
  WaitingNoResponse,
  Holding,
  Complete,
  Failed,
  Aborted,
};

static constexpr size_t TIGO_TRACE_REPLAY_MAX_STEPS = 64;
static constexpr size_t TIGO_TRACE_REPLAY_MAX_PAYLOAD = 64;

struct TraceReplayStep {
  uint32_t offsetMs;
  uint16_t targetGatewayId;
  uint16_t typeCode;
  uint16_t expectedType;
  uint16_t rfNodeId;
  uint8_t expectedPvSubcmd;
  uint8_t expectedCredit;
  uint8_t block;
  uint8_t payloadLen;
  TraceReplayAction action;
  TraceReplayOutcome outcome;
  TraceReplayRisk risk;
  uint8_t payload[TIGO_TRACE_REPLAY_MAX_PAYLOAD];
};

struct TraceReplayResult {
  bool valid;
  bool ok;
  TraceReplayOutcome actualOutcome;
  uint32_t actualStartOffsetMs;
  uint32_t completedOffsetMs;
  int32_t startLateMs;
  int32_t interStepLateMs;
  uint32_t dispatchPreparationMs;
  uint16_t responseType;
  uint16_t responseGatewayId;
  uint8_t responseDsn;
  uint8_t responseSubcmd;
  uint8_t responseCredit;
};

static constexpr size_t TIGO_RAW_FRAME_CAPTURE_QUEUE_LEN = 8;

struct RawFrameCaptureItem {
  uint16_t gatewayId;
  uint16_t addrRaw;
  uint16_t typeCode;
  uint16_t payloadLen;
  uint32_t deviceMs;
  bool fromGateway;
  bool crcOk;
  uint8_t payload[MAX_FRAME_PAYLOAD];
};

static constexpr size_t TIGO_RADIO_DESCRIPTOR_LEN = 36;

struct TapCommandState {
  TapCommandKind kind;
  TapCommandPhase phase;
  uint16_t expectedType;
  uint16_t expectedGatewayId;
  uint32_t deadlineMs;
  uint32_t nextActionMs;
  uint16_t arg0;
  uint16_t arg1;
  uint16_t arg2;
  uint8_t arg3;
  uint8_t expectedPvResponseSubcmd;
  uint8_t expectedDsn;
  uint8_t attempt;
  bool expectedRadioDescriptorValid;
  uint8_t expectedRadioDescriptor[TIGO_RADIO_DESCRIPTOR_LEN];
  char ackHex[192];
  char status[96];
  char discoveredLongAddr[17];
};

struct ActivePvEnvelope {
  bool valid;
  uint8_t txBuffersFree;
  uint8_t subcmd;
  uint8_t dsn;
  const uint8_t* body;
  size_t bodyLen;
};

struct PassivePvExpectation {
  bool valid;
  uint8_t responseSubcmd;
  uint8_t dsn;
  uint16_t gatewayId;
  uint32_t observedMs;
};

static constexpr size_t MAX_PASSIVE_PV_EXPECTATIONS = 4;
static constexpr uint32_t PASSIVE_PV_EXPECTATION_TIMEOUT_MS = 15000UL;

static const size_t MAX_INTERESTING_FRAMES = 80;
static const size_t INTERESTING_FRAME_REASON_LEN = 40;
static const size_t INTERESTING_FRAME_PAYLOAD_PREVIEW_BYTES = 96;
static const size_t INTERESTING_FRAME_PAYLOAD_HEX_LEN = (INTERESTING_FRAME_PAYLOAD_PREVIEW_BYTES * 2) + 1;

struct InterestingFrame {
  bool valid;
  bool crcOk;
  bool fromGateway;
  bool payloadTruncated;
  uint32_t seq;
  uint32_t ms;
  uint32_t frameCounter;
  uint16_t addrRaw;
  uint16_t gatewayId;
  uint16_t typeCode;
  uint16_t payloadLen;
  char reason[INTERESTING_FRAME_REASON_LEN];
  char payloadHex[INTERESTING_FRAME_PAYLOAD_HEX_LEN];
};

static const uint32_t TIGO_PERSISTENT_STATE_SAVE_DEBOUNCE_MS = 60000UL;
static const char* TIGO_PERSISTENT_STATE_STORAGE_KEY = TIGO_PREFS_STATE_KEY;
static const char* TIGO_MQTT_SETTINGS_STORAGE_KEY = TIGO_PREFS_MQTT_KEY;
static const char* TIGO_POLLING_SETTINGS_STORAGE_KEY = TIGO_PREFS_POLLING_KEY;
static const char* TIGO_MQTT_MIGRATION_STORAGE_KEY = TIGO_PREFS_MQTT_MIGRATION_KEY;
static const char* TIGO_RADIO_IDENTITY_STORAGE_KEY = TIGO_PREFS_RADIO_IDENTITY_KEY;
static const uint32_t TIGO_MQTT_SETTINGS_MAGIC = 0x4D515454UL; // 'MQTT'
static const uint32_t TIGO_POLLING_SETTINGS_MAGIC = 0x504F4C4CUL; // 'POLL'
static const uint32_t TIGO_RADIO_IDENTITY_MAGIC = 0x52464944UL; // 'RFID'
static const uint32_t TIGO_BOOT_JOURNAL_MAGIC = 0x424A524EUL; // 'BJRN'
static const uint32_t TIGO_BOOT_OPTIONS_MAGIC = 0x424F5054UL; // 'BOPT'
static const uint32_t TIGO_MQTT_MIGRATION_MAGIC = 0x4D494752UL; // 'MIGR'
static const uint16_t TIGO_MQTT_MIGRATION_VERSION = 1;
static const uint8_t TIGO_MQTT_MIGRATION_INVALID_NODE_STATUS = 0x01;
static const uint8_t TIGO_MQTT_MIGRATION_PANEL_TELEMETRY = 0x02;
static const uint8_t TIGO_MQTT_MIGRATION_SYSTEM_STATUS = 0x04;
static const size_t TIGO_JOIN_SEED_LEN = 24;
static const uint8_t TIGO_RADIO_IDENTITY_HAS_WORKING_DESCRIPTOR = 0x01;
static const uint8_t TIGO_RADIO_IDENTITY_HAS_JOIN_SEED = 0x02;
static const uint8_t TIGO_RADIO_IDENTITY_HAS_ROLLBACK_DESCRIPTOR = 0x04;
static const size_t TIGO_MQTT_HOST_LEN = 64;
static const size_t TIGO_MQTT_BASE_TOPIC_LEN = 64;
static const size_t TIGO_MQTT_USERNAME_LEN = 40;
static const size_t TIGO_MQTT_PASSWORD_LEN = 64;

struct MqttRuntimeSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t port;
  char host[TIGO_MQTT_HOST_LEN];
  char baseTopic[TIGO_MQTT_BASE_TOPIC_LEN];
  char username[TIGO_MQTT_USERNAME_LEN];
  char password[TIGO_MQTT_PASSWORD_LEN];
};

struct PollingRuntimeSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t enabled;
  uint8_t reserved;
};

struct MqttTopicMigrationSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t completedFlags;
  uint8_t reserved;
};

struct RadioIdentitySettings {
  uint32_t magic;
  uint16_t version;
  uint8_t flags;
  uint8_t reserved;
  uint8_t workingDescriptor[TIGO_RADIO_DESCRIPTOR_LEN];
  char workingTapLongAddr[17];
  uint8_t rollbackDescriptor[TIGO_RADIO_DESCRIPTOR_LEN];
  char rollbackTapLongAddr[17];
  uint8_t joinSeed[TIGO_JOIN_SEED_LEN];
  uint32_t joinSeedProfileFingerprint;
  uint32_t checksum;
};

struct BootOptionsSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t cursorStrategy;
  uint8_t reserved;
  uint32_t checksum;
};

struct BootMutationEntry {
  uint32_t epoch;
  uint16_t typeCode;
  uint8_t subcommand;
  uint8_t result;
  uint16_t beforeGatewayId;
  uint16_t requestedGatewayId;
  uint16_t confirmedGatewayId;
  uint16_t beforeNetworkConfirmed;
  uint16_t afterNetworkConfirmed;
  uint32_t beforeNodeTableHash;
  uint32_t afterNodeTableHash;
};

static constexpr size_t TIGO_BOOT_MUTATION_JOURNAL_ENTRIES = 12;

struct BootJournalSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t lastWorkingGatewayId;
  char tapLongAddr[17];
  char tapFirmware[72];
  uint32_t radioDescriptorFingerprint;
  uint32_t nodeTableHash;
  uint8_t networkStatusValid;
  uint8_t networkMode;
  uint16_t networkCountdown;
  uint16_t networkFlags;
  uint16_t networkConfirmed;
  uint16_t networkExpected;
  uint16_t packetCursor;
  uint32_t lastPowerEpoch;
  uint32_t lastReleasedEpoch;
  uint8_t lastBootPath;
  uint8_t lastTapState;
  uint8_t lastCursorStrategy;
  uint8_t transactionState;
  uint16_t transactionBeforeId;
  uint16_t transactionRequestedId;
  uint16_t transactionConfirmedId;
  uint16_t transactionType;
  uint8_t rollbackNeeded;
  uint8_t mutationHead;
  uint8_t mutationCount;
  uint8_t reserved;
  char lastMutation[32];
  BootMutationEntry mutations[TIGO_BOOT_MUTATION_JOURNAL_ENTRIES];
  uint32_t checksum;
};

// ------------------------------------------------------------
// Main master/controller implementation
// ------------------------------------------------------------
class TigoTapMaster {
 public:
  TigoTapMaster()
      : gatewayId_(TIGO_GATEWAY_ID),
        nextPacketNumber_(TIGO_INITIAL_PACKET_NUMBER),
        lastRequestedPacketNumber_(0),
        awaitingReceiveResponse_(false),
        lastPollSentMs_(0),
        lastPeriodicPingMs_(0),
        lastPeriodicNodeTableMs_(0),
        lastPeriodicNetworkStatusMs_(0),
        lastPollTimeoutEventMs_(0),
        lastStateSaveMs_(0),
        framesRx_(0),
        framesCrcError_(0),
        lastTapResponseMs_(0),
        tapResponsesRx_(0),
        pollsSent_(0),
        pollTimeouts_(0),
        activePollingEnabled_(TIGO_RS485_ACTIVE_POLLING),
        wifiConnected_(false),
        apMode_(false),
        lastWifiStatus_(WL_IDLE_STATUS),
        recentEventHead_(0),
        mqttClient_(wifiClient_),
        mqttConnected_(false),
        mqttMigrationFlags_(0),
        lastMqttConnectAttemptMs_(0),
        lastMqttHeartbeatPublishMs_(0),
        lastMqttStatusPublishMs_(0),
        lastMqttTelemetryPublishMs_(0),
        lastLegacyDiscoveryStepMs_(0),
        lastLegacyStateClearMs_(0),
        lastInvalidNodeStatusClearMs_(0),
        lastDeprecatedPanelTelemetryClearMs_(0),
        lastDeprecatedSystemStatusClearMs_(0),
        lastLegacyDiscoveryFailureLogMs_(0),
        lastSerialWifiStatusMs_(0),
        lastSerialPowerStatusMs_(0),
        lastSerialRxPollStatusMs_(0),
        lastWebRequestMs_(0),
        lastWebServerRecoverMs_(0),
        interestingFrameSeq_(0),
        interestingFrameHead_(0),
        interestingFrameDropped_(0),
        lastInterestingGatewayId_(0),
        jsonBuildTarget_(nullptr),
        otaInProgress_(false),
        pollingEnabledBeforeOta_(false),
        nodeWakeActive_(false),
        nodeWakeCompleted_(false),
        nodeWakeLearnActive_(false),
        nodeWakeLearnWaitCountdown_(false),
        nodeWakeSkipLearn_(false),
        nodeWakeLearnRestartAfterPvRun_(false),
        forceLearnUntilMs_(0),
        nodeSeedState_(NodeSeedState::Idle),
        nodeSeedAwaitingCommand_(false),
        nodeWakeConfigFallbackActive_(false),
        nodeWakeForcePvConfig_(false),
        nodeWakeStep_(0),
        nodeWakeNextActionMs_(0),
        nodeWakeLearnStartedMs_(0),
        nodeWakeLearnLastNetworkStatusMs_(0),
        nodeWakeLearnLastJoinSeedMs_(0),
        nodeWakeLearnWarmupStep_(0),
        rfNodePromotionState_(RfNodePromotionState::Idle),
        rfNodePromotionNodeId_(0),
        nodeSeedNextPanelIndex_(0),
        nodeSeedPanelCount_(0),
        nodeSeedChunksTotal_(0),
        nodeSeedChunksAcked_(0),
        nodeSeedRetryCount_(0),
        nodeSeedVerifyStart_(0),
        nodeSeedVerifiedReadbackEntries_(0),
        lastNodeTableStart_(0),
        lastNodeTableEntryCount_(0),
        lastNodeTableMs_(0),
        lastNodeWakeAttemptMs_(0),
        legacyDiscoveryPublished_(false),
        legacyDiscoveryClearIndex_(0),
        invalidNodeStatusClearIndex_(0),
        deprecatedPanelTelemetryClearIndex_(0),
        deprecatedSystemStatusClearIndex_(0),
        legacyDiscoveryPublishSlotIndex_(0),
        legacyStateTopicsCleared_(false),
        invalidNodeStatusTopicsCleared_(false),
        deprecatedPanelTelemetryTopicsCleared_(false),
        deprecatedSystemStatusTopicsCleared_(false),
        legacyStateDirty_(true),
        statusDirty_(true) {
    gatewayLongAddr_[0] = '\0';
    memset(nodeMap_, 0, sizeof(nodeMap_));
    memset(powerSlots_, 0, sizeof(powerSlots_));
    memset(events_, 0, sizeof(events_));
    memset(interestingFrames_, 0, sizeof(interestingFrames_));
    memset(passivePvExpectations_, 0, sizeof(passivePvExpectations_));
    lastNodeTableAckHex_[0] = '\0';
    lastNetworkStatusAckHex_[0] = '\0';
    lastRadioConfigAckHex_[0] = '\0';
    lastPvSubcommandRequestHex_[0] = '\0';
    lastPvSubcommandAckHex_[0] = '\0';
    lastPvAckBodyHex_[0] = '\0';
    lastSeedError_[0] = '\0';
    lastCommandName_[0] = '\0';
    lastCommandMessage_[0] = '\0';
    rfNodePromotionLongAddr_[0] = '\0';
    webVersionText_[0] = '\0';
    platformRuntime_.fillResetReason(lastResetReason_, sizeof(lastResetReason_));
    mqttClientId_[0] = '\0';
    resetMqttSettingsToDefaults();
    resetRadioIdentitySettings();
    resetBootOptionsSettings();
    resetBootJournalSettings();
    memset(currentRadioDescriptor_, 0, sizeof(currentRadioDescriptor_));
    currentRadioDescriptorTapLongAddr_[0] = '\0';
    memset(&commandState_, 0, sizeof(commandState_));
    commandState_.kind = TapCommandKind::None;
    commandState_.phase = TapCommandPhase::Idle;
    panelFieldCount_ = panelFieldCountClamped();
    for (size_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
      makeDefaultPanelLabel(i, panelMap_[i].label, sizeof(panelMap_[i].label));
      panelMap_[i].longAddr[0] = '\0';
    }
  }

  void begin() {
    g_tigoMqttCallbackTarget = this;
    persistentStoreReady_ = platformRuntime_.persistentStore().begin();
    loadStateFromPersistentStore();
    persistedGatewayId_ = gatewayId_;
    persistedPacketCursor_ = nextPacketNumber_;
    confirmedPacketCursor_ = nextPacketNumber_;
    loadBootOptionsFromPersistentStore();
    loadBootJournalFromPersistentStore();
    reconcilePersistentStateWithBootJournal();
    loadPollingSettingsFromPersistentStore();
    loadMqttSettingsFromPersistentStore();
    loadMqttTopicMigrationSettingsFromPersistentStore();
    loadRadioIdentityFromPersistentStore();

    uint8_t bootstrapLongAddr[8];
    if (countPanelMapLongAddrs() == 0 &&
        hex16ToBytes(TIGO_BOOTSTRAP_OPTIMIZER_LONG_ADDR_HEX, bootstrapLongAddr)) {
      panelFieldCount_ = 1;
      copyString(panelMap_[0].longAddr, sizeof(panelMap_[0].longAddr),
                 TIGO_BOOTSTRAP_OPTIMIZER_LONG_ADDR_HEX);
      markPersistentStateDirty();
      flushPersistentState(true);
      addEvent("empty panel map bootstrapped with optimizer %s",
               TIGO_BOOTSTRAP_OPTIMIZER_LONG_ADDR_HEX);
    }

    platformRuntime_.rs485Port().begin();

    addEvent("boot; esp32-c6 uart=%d rs485 rx=%d tx=%d dir pin=%d",
             (int)TIGO_RS485_UART_PORT,
             (int)TIGO_RS485_RX_PIN,
             (int)TIGO_RS485_TX_PIN,
             (int)TIGO_RS485_DIR_PIN);
    if (!persistentStoreReady_) {
      addEvent("prefs unavailable: namespace=%s", TIGO_PREFS_NAMESPACE);
    }
    addEvent("usb serial baud=%lu", (unsigned long)TIGO_USB_BAUD);

    connectWifi();
    setupMqtt();
    setupWeb();
    setupOta();

    if (TIGO_ENABLE_ENUM_AT_BOOT) {
      bootEnumeratePending_ = true;
    }

    if (gatewayId_ == 0) {
      gatewayId_ = TIGO_GATEWAY_ID;
    }
    if (TIGO_RESET_PACKET_COUNTER_AT_BOOT) {
      nextPacketNumber_ = TIGO_INITIAL_PACKET_NUMBER;
    }
    applyBootCursorStrategy();
    bootStartedMs_ = platformMillis();

    if (TIGO_READ_ONLY_WARM_ATTACH) {
      bootCcaStep_ = UINT8_MAX;
      bootCcaWaitingStep_ = UINT8_MAX;
      bootCcaNextActionMs_ = 0;
      bootCcaRetryCount_ = 0;
      bootCcaWarmAttachTried_ = false;
      bootCcaWarmAttachPending_ = false;
      beginReadOnlyWarmAttach(false);
    } else if (TIGO_CCA_COMPAT_BOOT_SEQUENCE) {
      bootCcaStep_ = 0;
      bootCcaWaitingStep_ = UINT8_MAX;
      bootCcaNextActionMs_ = 0;
      bootCcaRetryCount_ = 0;
      bootCcaWarmAttachTried_ = false;
      bootCcaWarmAttachPending_ = false;
      bootVersionPending_ = false;
      bootRadioConfigPending_ = false;
      bootGatewaySelector0Pending_ = false;
      bootNetworkStatusPending_ = false;
      bootNodeTablePending_ = false;
      bootGatewaySelector1Pending_ = false;
      bootNodeTableEndPending_ = false;
    } else {
      bootCcaStep_ = UINT8_MAX;
      bootCcaWaitingStep_ = UINT8_MAX;
      bootCcaNextActionMs_ = 0;
      bootCcaRetryCount_ = 0;
      bootVersionPending_ = TIGO_REQUEST_VERSION_AT_BOOT;
      bootRadioConfigPending_ = TIGO_REQUEST_RADIO_CONFIG_AT_BOOT;
      bootGatewaySelector0Pending_ = true;
      bootNetworkStatusPending_ = TIGO_REQUEST_NETWORK_STATUS_AT_BOOT;
      bootNodeTablePending_ = TIGO_REQUEST_NODE_TABLE_AT_BOOT;
      bootGatewaySelector1Pending_ = true;
      bootNodeTableEndPending_ = TIGO_REQUEST_NODE_TABLE_AT_BOOT;
    }
  }

  void loop() {
    // Give an in-flight TAP reply priority over synchronous Wi-Fi work.
    // WebServer and PubSubClient writes can occasionally block while a
    // complete response is already waiting in the UART.
    serviceSerial();
    serviceWebServer();
    if (rebootRequested_ && elapsed(platformMillis(), rebootAtMs_) < 0x80000000UL) {
      Serial.println("OTX reboot requested from web");
      Serial.flush();
      ESP.restart();
    }
    if (TIGO_ENABLE_OTA && !rs485ReplyPending()) {
      ArduinoOTA.handle();
      if (otaInProgress_) {
        platformYield();
        return;
      }
    }
    if (!rs485ReplyPending()) {
      maintainWifiAndWeb();
      maintainMqtt();
      flushPendingRawFrameCapture();
      serviceWebServer();
    }
    serviceSerial();
    serviceWebServer();

    const uint32_t now = platformMillis();
    recoverWebServerIfIdle(now);
    printSerialWifiStatus(now);
    processTapCommand(now);
    processTraceReplay(now);
    processTraceReplayHold(now);
    processBootCommands(now);
    processNodeWakeSequence(now);

    if (gatewayId_ != 0) {
      if (activePollingEnabled_ &&
          awaitingReceiveResponse_ &&
          elapsed(now, lastPollSentMs_) > TIGO_RS485_POLL_TIMEOUT_MS) {
        awaitingReceiveResponse_ = false;
        ++pollTimeouts_;
        if (lastPollTimeoutEventMs_ == 0 ||
            elapsed(now, lastPollTimeoutEventMs_) >= TIGO_RS485_POLL_TIMEOUT_EVENT_EVERY_MS) {
          lastPollTimeoutEventMs_ = now;
          addEvent("poll timeouts=%lu; gateway=%04X packet=%04X",
                   (unsigned long)pollTimeouts_,
                   gatewayId_,
                   lastRequestedPacketNumber_);
        }
      }

      if (activePollingEnabled_ &&
          !replayExclusiveMode_ &&
          !warmAttachTrafficGateActive() &&
          !tapCommandActive() &&
          !awaitingReceiveResponse_ &&
          elapsed(now, lastPollSentMs_) >= TIGO_RS485_POLL_INTERVAL_MS) {
        if (TIGO_PING_EVERY_MS > 0 && elapsed(now, lastPeriodicPingMs_) >= TIGO_PING_EVERY_MS) {
          ping();
          lastPeriodicPingMs_ = now;
        }
        if (TIGO_NODE_TABLE_EVERY_MS > 0 && elapsed(now, lastPeriodicNodeTableMs_) >= TIGO_NODE_TABLE_EVERY_MS) {
          requestNodeTable(0);
          lastPeriodicNodeTableMs_ = now;
        }
        if (TIGO_NETWORK_STATUS_EVERY_MS > 0 && elapsed(now, lastPeriodicNetworkStatusMs_) >= TIGO_NETWORK_STATUS_EVERY_MS) {
          requestNetworkStatus();
          lastPeriodicNetworkStatusMs_ = now;
        }

        sendReceiveRequest();
      }
    }

    flushPersistentState(false, now);
    flushBootJournal(false, now);

    serviceWebServer();
    if (!rs485ReplyPending()) {
      flushPendingRawFrameCapture();
      publishMqttPeriodic(now);
    }
    serviceWebServer();
    platformYield();
  }

  // --------------------------
  // Public data for web API
  // --------------------------
  uint16_t gatewayId() const { return gatewayId_; }
  uint16_t nextPacketNumber() const { return nextPacketNumber_; }
  const char* gatewayLongAddr() const { return gatewayLongAddr_; }
  uint32_t framesRx() const { return framesRx_; }
  uint32_t framesCrcError() const { return framesCrcError_; }
  uint32_t pollsSent() const { return pollsSent_; }
  uint32_t pollTimeouts() const { return pollTimeouts_; }
  bool activePollingEnabled() const { return activePollingEnabled_; }
  bool wifiConnected() const { return wifiConnected_; }
  bool apMode() const { return apMode_; }
  const char* versionText() const { return webVersionText_; }
  const char* lastNodeTableAckHex() const { return lastNodeTableAckHex_; }
  const char* lastNetworkStatusAckHex() const { return lastNetworkStatusAckHex_; }
  const char* lastRadioConfigAckHex() const { return lastRadioConfigAckHex_; }
  const char* lastPvSubcommandRequestHex() const { return lastPvSubcommandRequestHex_; }
  const char* lastPvSubcommandAckHex() const { return lastPvSubcommandAckHex_; }
  const char* lastPvAckBodyHex() const { return lastPvAckBodyHex_; }
  uint8_t lastPvSubcommand() const { return lastPvSubcommand_; }
  uint8_t lastPvAckStatusFlags() const { return lastPvAckStatusFlags_; }
  uint8_t lastPvAckResponseSubcmd() const { return lastPvAckResponseSubcmd_; }
  bool lastNetworkStatusValid() const { return lastNetworkStatusValid_; }
  uint8_t lastNetworkMode() const { return lastNetworkMode_; }
  uint16_t lastNetworkCountdown() const { return lastNetworkCountdown_; }
  uint16_t lastNetworkConfiguredNodes() const { return lastNetworkExpectedNodes_; }
  uint16_t lastNetworkActiveNodes() const { return lastNetworkConfirmedNodes_; }
  bool tapCommandBusy() const { return tapCommandActive(); }
  const char* activeTapCommandName() const { return tapCommandName(commandState_.kind); }
  const char* activeTapCommandStatus() const { return commandState_.status; }
  uint16_t nodeMapCount() const { return countValidNodeMap(); }
  uint16_t powerCount() const { return countValidPower(); }
  uint16_t configuredOptimizerCount() const { return panelFieldCount_; }
  void dispatchMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    handleMqttMessage(topic, payload, length);
  }

  const NodeMapEntry* nodeMap() const { return nodeMap_; }
  const PowerReport* powerSlots() const { return powerSlots_; }
  const RecentEvent* events() const { return events_; }

  const char* tapBootPathName(TapBootPath path) const {
    switch (path) {
      case TapBootPath::PassiveListen: return "passive_listen";
      case TapBootPath::ReadOnlyWarmAttach: return "read_only_warm_attach";
      case TapBootPath::ReadOnlyPolling: return "read_only_polling";
      case TapBootPath::TargetedRecovery: return "targeted_recovery";
      case TapBootPath::LegacyCcaReplay: return "legacy_cca_replay";
      default: return "unknown";
    }
  }

  const char* traceReplayStateName(TraceReplayState state) const {
    switch (state) {
      case TraceReplayState::Loaded: return "loaded";
      case TraceReplayState::Running: return "running";
      case TraceReplayState::WaitingCommand: return "waiting_command";
      case TraceReplayState::WaitingNoResponse: return "waiting_no_response";
      case TraceReplayState::Holding: return "holding";
      case TraceReplayState::Complete: return "complete";
      case TraceReplayState::Failed: return "failed";
      case TraceReplayState::Aborted: return "aborted";
      default: return "idle";
    }
  }

  const char* traceReplayActionName(TraceReplayAction action) const {
    switch (action) {
      case TraceReplayAction::ReceiveBootstrap: return "receive_bootstrap";
      case TraceReplayAction::PvSubcommand: return "pv_subcommand";
      default: return "simple_frame";
    }
  }

  const char* traceReplayOutcomeName(TraceReplayOutcome outcome) const {
    switch (outcome) {
      case TraceReplayOutcome::NoResponse: return "no_response";
      case TraceReplayOutcome::TapAck: return "tap_ack";
      case TraceReplayOutcome::RfResponse: return "rf_response";
      case TraceReplayOutcome::LocalTapAck: return "local_tap_ack";
      default: return "response";
    }
  }

  const char* traceReplayRiskName(TraceReplayRisk risk) const {
    switch (risk) {
      case TraceReplayRisk::ReadOnly: return "read_only";
      case TraceReplayRisk::ActiveRf: return "active_rf";
      case TraceReplayRisk::StateChanging: return "state_changing";
      default: return "unknown";
    }
  }

  TraceReplayRisk traceReplayRiskFor(TraceReplayAction action, uint16_t typeCode,
                                     const uint8_t* payload = nullptr,
                                     size_t payloadLen = 0) const {
    if (action == TraceReplayAction::SimpleFrame) {
      if (typeCode == 0x0B00 && payload != nullptr && payloadLen == 1 &&
          payload[0] <= 0x01) {
        return TraceReplayRisk::StateChanging;
      }
      if (typeCode == 0x0010 || typeCode == 0x0012 ||
          typeCode == 0x0014 || typeCode == 0x003C) {
        return TraceReplayRisk::StateChanging;
      }
      if (typeCode == 0x0038 || typeCode == 0x003A ||
          typeCode == 0x000A || typeCode == 0x000E ||
          typeCode == 0x0E02 || typeCode == 0x0148) {
        return TraceReplayRisk::ReadOnly;
      }
      return TraceReplayRisk::Unknown;
    }
    if (action == TraceReplayAction::PvSubcommand) {
      const uint8_t subcmd = (uint8_t)(typeCode & 0xFFU);
      if (subcmd == 0x06 || subcmd == 0x17) {
        return TraceReplayRisk::ActiveRf;
      }
      if (subcmd == 0x13 || subcmd == 0x22 || subcmd == 0x29 ||
          subcmd == 0x2B || subcmd == 0x2D || subcmd == 0x41) {
        return TraceReplayRisk::StateChanging;
      }
      if (subcmd == 0x0D || subcmd == 0x26 || subcmd == 0x2E) {
        return TraceReplayRisk::ReadOnly;
      }
      return TraceReplayRisk::Unknown;
    }
    return action == TraceReplayAction::ReceiveBootstrap
        ? TraceReplayRisk::ReadOnly : TraceReplayRisk::Unknown;
  }

  const char* tapObservedStateName(TapObservedState state) const {
    switch (state) {
      case TapObservedState::FactoryOrUnaddressed: return "factory_or_unaddressed";
      case TapObservedState::AddressedRadioUnknown: return "addressed_radio_unknown";
      case TapObservedState::WarmOperational: return "warm_operational";
      case TapObservedState::NodeTableEmpty: return "node_table_empty";
      case TapObservedState::NodeTablePending: return "node_table_pending";
      case TapObservedState::PartiallyConfirmed: return "partially_confirmed";
      case TapObservedState::FullyConfirmedNoTelemetry: return "fully_confirmed_no_telemetry";
      case TapObservedState::TelemetryActiveRsdLocked: return "telemetry_active_rsd_locked";
      case TapObservedState::FullyReleasedStable: return "fully_released_stable";
      default: return "unknown_unreachable";
    }
  }

  const char* bootCursorStrategyName(BootCursorStrategy strategy) const {
    switch (strategy) {
      case BootCursorStrategy::Zero: return "zero";
      case BootCursorStrategy::CcaBootstrap: return "cca_bootstrap";
      default: return "persisted";
    }
  }

  const char* warmAttachPhaseName(WarmAttachPhase phase) const {
    switch (phase) {
      case WarmAttachPhase::PassiveListen: return "passive_listen";
      case WarmAttachPhase::ProbeCandidate: return "probe_candidate";
      case WarmAttachPhase::FactoryAssign: return "factory_assign";
      case WarmAttachPhase::FactoryVerify: return "factory_verify";
      case WarmAttachPhase::Version: return "version";
      case WarmAttachPhase::RadioSelector0: return "radio_selector_0";
      case WarmAttachPhase::RadioSelector1: return "radio_selector_1";
      case WarmAttachPhase::NetworkStatus: return "network_status";
      case WarmAttachPhase::NodeTable: return "node_table";
      case WarmAttachPhase::CursorBootstrapZero: return "cursor_bootstrap_0000";
      case WarmAttachPhase::CursorBootstrapEeee: return "cursor_bootstrap_eeee";
      case WarmAttachPhase::Complete: return "complete";
      case WarmAttachPhase::Failed: return "failed";
      default: return "idle";
    }
  }

  const char* tapStateRecommendedAction(TapObservedState state) const {
    switch (state) {
      case TapObservedState::UnknownUnreachable: return "probe_known_ids";
      case TapObservedState::FactoryOrUnaddressed: return "verified_address_assignment";
      case TapObservedState::AddressedRadioUnknown: return "read_radio_profile";
      case TapObservedState::WarmOperational: return "read_table_then_poll";
      case TapObservedState::NodeTableEmpty: return "confirm_empty_then_explicit_seed";
      case TapObservedState::NodeTablePending: return "poll_and_explicit_learn_only";
      case TapObservedState::PartiallyConfirmed: return "preserve_and_poll";
      case TapObservedState::FullyConfirmedNoTelemetry: return "explicit_rsd_run_then_observe";
      case TapObservedState::TelemetryActiveRsdLocked: return "explicit_rsd_run_then_observe";
      case TapObservedState::FullyReleasedStable: return "poll_only";
      default: return "read_only_diagnostics";
    }
  }

  bool hasFreshPowerTelemetry(uint32_t now) const {
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (powerSlots_[i].valid &&
          elapsed(now, powerSlots_[i].updatedMs) <= TIGO_SAMPLE_FRESH_MS) {
        return true;
      }
    }
    return false;
  }

  bool hasFreshReleasedPower(uint32_t now) const {
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (powerSlots_[i].valid &&
          elapsed(now, powerSlots_[i].updatedMs) <= TIGO_SAMPLE_FRESH_MS &&
          powerSlots_[i].voutV >= TIGO_ELECTRICAL_RELEASE_MIN_VOUT_V) {
        return true;
      }
    }
    return false;
  }

  bool hasStableReleasedPower(uint32_t now) const {
    return hasFreshReleasedPower(now) &&
           releasedPowerEvidenceCount_ >= 2 &&
           firstReleasedPowerEvidenceMs_ != 0 &&
           lastReleasedPowerEvidenceMs_ != 0 &&
           elapsed(now, lastReleasedPowerEvidenceMs_) <= TIGO_SAMPLE_FRESH_MS &&
           elapsed(now, firstReleasedPowerEvidenceMs_) >= TIGO_ELECTRICAL_RELEASE_STABLE_MS;
  }

  TapObservedState classifyTapState(uint32_t now) const {
    if (lastTapResponseMs_ == 0 ||
        elapsed(now, lastTapResponseMs_) > TIGO_TAP_LINK_FRESH_MS) {
      return TapObservedState::UnknownUnreachable;
    }
    if (gatewayId_ == TIGO_ENUM_ID) {
      return TapObservedState::FactoryOrUnaddressed;
    }
    if (!currentRadioDescriptorValid_) {
      return TapObservedState::AddressedRadioUnknown;
    }
    if (lastNodeTableMs_ == 0) {
      return TapObservedState::WarmOperational;
    }
    const uint16_t valid = countValidNodeMap();
    const uint16_t pending = countPendingNodeMap();
    const bool networkStatusFresh = lastNetworkStatusValid_ &&
        lastNetworkStatusMs_ != 0 &&
        elapsed(now, lastNetworkStatusMs_) <= TIGO_NETWORK_STATUS_STATE_FRESH_MS;
    const uint16_t confirmed = networkStatusFresh
        ? lastNetworkConfirmedNodes_ : countConfirmedNodeMap();
    const uint16_t expected = networkStatusFresh && lastNetworkExpectedNodes_ > 0
        ? lastNetworkExpectedNodes_ : valid;
    if (valid == 0) {
      return TapObservedState::NodeTableEmpty;
    }
    if (pending > 0) {
      return confirmed == 0
          ? TapObservedState::NodeTablePending
          : TapObservedState::PartiallyConfirmed;
    }
    if (expected > 0 && confirmed < expected) {
      return TapObservedState::PartiallyConfirmed;
    }
    if (!hasFreshPowerTelemetry(now)) {
      return TapObservedState::FullyConfirmedNoTelemetry;
    }
    return hasStableReleasedPower(now)
        ? TapObservedState::FullyReleasedStable
        : TapObservedState::TelemetryActiveRsdLocked;
  }

  uint32_t currentNodeTableHash() const {
    uint8_t encoded[MAX_NODE_MAP * 10U];
    size_t len = 0;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid || len + 10U > sizeof(encoded)) {
        continue;
      }
      uint8_t longAddr[8];
      if (!hex16ToBytes(nodeMap_[i].longAddr, longAddr)) {
        continue;
      }
      memcpy(encoded + len, longAddr, sizeof(longAddr));
      len += sizeof(longAddr);
      encoded[len++] = (uint8_t)(nodeMap_[i].rawNodeId >> 8);
      encoded[len++] = (uint8_t)(nodeMap_[i].rawNodeId & 0xFF);
    }
    return fnv1a32(encoded, len);
  }

  bool shouldRunLateAddressDance() const {
    // The known H1.0004 / GW-H158.7.00 TAP operates without this dance. Keep
    // the experimental sequence behind the explicit build flag until a trace
    // proves a firmware-specific requirement.
    return TIGO_CCA_ENABLE_LATE_ADDRESS_DANCE;
  }

  // --------------------------
  // Explicit actions
  // --------------------------
  bool ping() {
    if (!beginTapCommand(TapCommandKind::Ping, "ping")) {
      return false;
    }
    sendGatewayFrame(gatewayId_, 0x003A, nullptr, 0);
    setTapCommandWait(TapCommandPhase::WaitingFrame, 0x003B, gatewayId_, 500,
                      "waiting for read-only network probe");
    return true;
  }

  bool requestRsdControl(bool run) {
    return sendRsdControlFrame(run,
                               run ? "waiting for RSD RUN ack" : "waiting for RSD STOP ack",
                               TapCommandKind::RsdControl);
  }

  bool requestVersion() {
    if (!beginTapCommand(TapCommandKind::Version, "version")) {
      return false;
    }
    sendGatewayFrame(gatewayId_, 0x000A, nullptr, 0);
    setTapCommandWait(TapCommandPhase::WaitingFrame, 0x000B, gatewayId_, 600, "waiting for version response");
    return true;
  }

  bool requestNodeTable(uint16_t startIndex) {
    if (!beginTapCommand(TapCommandKind::NodeTable, "node table")) {
      return false;
    }
    uint8_t data[2];
    data[0] = (uint8_t)(startIndex >> 8);
    data[1] = (uint8_t)(startIndex & 0xFF);
    commandState_.expectedPvResponseSubcmd = 0x27;
    sendQueuedPvCommand(0x26, data, sizeof(data), lastNodeTableAckHex_, sizeof(lastNodeTableAckHex_), "waiting for node table ack");
    return true;
  }

  bool requestNetworkStatus() {
    if (!beginTapCommand(TapCommandKind::NetworkStatus, "network status")) {
      return false;
    }
    commandState_.expectedPvResponseSubcmd = 0x2F;
    sendQueuedPvCommand(0x2E, nullptr, 0, lastNetworkStatusAckHex_, sizeof(lastNetworkStatusAckHex_), "waiting for network status ack");
    return true;
  }

  bool requestPvRunState(bool enabled) {
    uint8_t data[2];
    const uint16_t pvOff = enabled ? 0x0000U : 0x0001U;
    data[0] = (uint8_t)(pvOff >> 8);
    data[1] = (uint8_t)(pvOff & 0xFF);
    return sendPvSubcommand(0x22, data, sizeof(data), 0x23);
  }

  bool requestPvBroadcastEmpty() {
    return sendPvSubcommand(0x22, nullptr, 0, 0x23);
  }

  bool requestCcaNodeWindow(uint16_t firstNodeId) {
    // Captured healthy CCA sessions place 01 <node:u16> between the first
    // node-table page and its end page. Its exact RF meaning is still unknown,
    // so preserve the byte sequence without presenting it as a user control.
    uint8_t data[3] = {
      0x01,
      (uint8_t)(firstNodeId >> 8),
      (uint8_t)(firstNodeId & 0xFF)
    };
    return sendPvSubcommand(0x22, data, sizeof(data), 0x23);
  }

  bool requestGatewayRadioConfig(uint16_t selector = 0x0001) {
    if (!beginTapCommand(TapCommandKind::RadioConfig, "radio config")) {
      return false;
    }
    uint8_t data[2] = {
      (uint8_t)(selector >> 8),
      (uint8_t)(selector & 0xFF)
    };
    commandState_.arg0 = 0;
    commandState_.expectedPvResponseSubcmd = 0x0E;
    sendQueuedPvCommand(0x0D, data, sizeof(data), lastRadioConfigAckHex_, sizeof(lastRadioConfigAckHex_), "waiting for radio config ack");
    return true;
  }

  bool writeGatewayRadioDescriptor(const uint8_t* descriptor) {
    if (descriptor == nullptr || !beginTapCommand(TapCommandKind::RadioConfig, "radio profile write")) {
      return false;
    }
    uint8_t data[2 + TIGO_RADIO_DESCRIPTOR_LEN];
    data[0] = 0x01;
    data[1] = 0x00;
    memcpy(data + 2, descriptor, TIGO_RADIO_DESCRIPTOR_LEN);
    commandState_.arg0 = 1;
    commandState_.expectedPvResponseSubcmd = 0x0E;
    commandState_.expectedRadioDescriptorValid = true;
    memcpy(commandState_.expectedRadioDescriptor, descriptor, TIGO_RADIO_DESCRIPTOR_LEN);
    sendQueuedPvCommand(0x0D, data, sizeof(data), lastRadioConfigAckHex_, sizeof(lastRadioConfigAckHex_),
                        "waiting for radio profile write ack");
    return true;
  }

  void requestRadioProfileReadbackForStore() {
    const uint8_t selector[] = {0x00, 0x01};
    commandState_.arg0 = 3;
    commandState_.expectedPvResponseSubcmd = 0x0E;
    sendQueuedPvCommand(0x0D, selector, sizeof(selector),
                        lastRadioConfigAckHex_, sizeof(lastRadioConfigAckHex_),
                        "verifying radio profile before STORE");
  }

  void requestRadioProfileStore() {
    static const uint8_t magic[] = {0x37, 0x24, 0x92, 0x66};
    commandState_.arg0 = 4;
    const uint16_t beforeId = bootJournal_.lastWorkingGatewayId != 0
        ? bootJournal_.lastWorkingGatewayId : gatewayId_;
    beginAddressTransaction(0x0012, beforeId, gatewayId_,
                            "radio profile STORE address transaction");
    recordMutationRequested(0x0012, 0, gatewayId_,
                            "radio profile STORE");
    if (!flushBootJournal(true)) {
      failTapCommand("radio profile STORE journal flush failed before transmit");
      return;
    }
    if (!sendGatewayFrame(gatewayId_, 0x0012, magic, sizeof(magic))) {
      failTapCommand("radio profile STORE blocked before transmit");
      return;
    }
    setTapCommandWait(TapCommandPhase::WaitingFrame, 0x0013, gatewayId_, 1000,
                      "waiting for radio profile STORE ack");
  }

  void requestGatewayHardResetAfterRadioStore() {
    static const uint8_t magic[] = {0x37, 0x24, 0x92, 0x66};
    commandState_.arg0 = 5;
    const uint16_t beforeId = bootJournal_.lastWorkingGatewayId != 0
        ? bootJournal_.lastWorkingGatewayId : gatewayId_;
    beginAddressTransaction(0x0010, beforeId, gatewayId_,
                            "radio profile APPLY address transaction");
    recordMutationRequested(0x0010, 0, gatewayId_,
                            "radio profile APPLY");
    if (!flushBootJournal(true)) {
      failTapCommand("gateway APPLY journal flush failed before transmit");
      return;
    }
    if (!sendGatewayFrame(0x0000, 0x0010, magic, sizeof(magic))) {
      failTapCommand("gateway APPLY blocked before transmit");
      return;
    }
    setTapCommandWait(TapCommandPhase::WaitingFrame, 0x0011, 0x0000, 1000,
                      "waiting for gateway hard reset ack");
  }

  bool requestGatewayJoinSeed() {
    if (!radioJoinSeedMatchesCurrentProfile()) {
      return false;
    }
    return sendPvSubcommand(0x41, radioJoinSeed_, sizeof(radioJoinSeed_), 0x41);
  }

  bool loadBootstrapRadioDescriptor(uint8_t* descriptor) {
    size_t len = 0;
    return descriptor != nullptr &&
           hexTextToBytes(TIGO_BOOTSTRAP_RADIO_DESCRIPTOR_HEX,
                          descriptor, TIGO_RADIO_DESCRIPTOR_LEN, &len) &&
           len == TIGO_RADIO_DESCRIPTOR_LEN &&
           radioDescriptorLooksValid(descriptor);
  }

  bool loadBootstrapRadioJoinSeed(uint8_t* seed) {
    size_t len = 0;
    return seed != nullptr &&
           hexTextToBytes(TIGO_BOOTSTRAP_RADIO_JOIN_SEED_HEX,
                          seed, TIGO_JOIN_SEED_LEN, &len) &&
           len == TIGO_JOIN_SEED_LEN;
  }

  bool requestGatewayLearnStart() {
    uint16_t expectedNodes = countPanelMapLongAddrs();
    if (expectedNodes == 0) {
      expectedNodes = lastNetworkExpectedNodes_;
    }
    if (expectedNodes == 0) {
      expectedNodes = nodeSeedPanelCount_;
    }
    if (expectedNodes == 0) {
      expectedNodes = countValidNodeMap();
    }
    if (expectedNodes == 0) {
      return false;
    }
    uint8_t data[] = {
      0xBA, 0xBE, 0x02, 0x03, 0x84,
      (uint8_t)(expectedNodes >> 8), (uint8_t)(expectedNodes & 0xFF),
      0x01, 0x00
    };
    return sendPvSubcommand(0x2D, data, sizeof(data), 0x2F);
  }

  bool requestGatewayLearnCancel() {
    static const uint8_t data[] = {
      0xBA, 0xBE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    return sendPvSubcommand(0x2D, data, sizeof(data), 0x2F);
  }

  bool requestGatewayTableClear() {
    static const uint8_t data[] = {0xBA, 0xBE};
    return sendPvSubcommand(0x2B, data, sizeof(data), 0x2C);
  }

  bool requestGatewayConfirmedNodeWrite(uint16_t nodeId, const char* longAddr) {
    uint8_t longAddrBytes[8];
    if (nodeId == 0 || longAddr == nullptr ||
        !hex16ToBytes(longAddr, longAddrBytes)) {
      return false;
    }
    uint8_t data[11];
    data[0] = 1;
    memcpy(data + 1, longAddrBytes, sizeof(longAddrBytes));
    data[9] = (uint8_t)(nodeId >> 8);
    data[10] = (uint8_t)(nodeId & 0xFF);
    return sendPvSubcommand(0x29, data, sizeof(data), 0x2A);
  }

  bool requestGatewayNodeSeedFromPanelMapChunk(uint16_t startPanelIndex) {
    constexpr uint16_t chunkSize =
        TIGO_NODE_SEED_CHUNK_SIZE == 0 ? 1 : TIGO_NODE_SEED_CHUNK_SIZE;
    uint8_t data[1 + (chunkSize * 10U)];
    uint8_t count = 0;
    size_t idx = 1;
    uint16_t configuredSeen = 0;
    char firstLongAddr[17] = "";
    const uint16_t maxPanels = TIGO_MAX_OPTIMIZERS;
    for (size_t i = 0; i < TIGO_MAX_OPTIMIZERS && configuredSeen < maxPanels && count < chunkSize; ++i) {
      if (panelMap_[i].longAddr[0] == '\0') {
        continue;
      }
      const uint16_t panelIndex = configuredSeen++;
      if (panelIndex < startPanelIndex) {
        continue;
      }
      uint8_t longAddr[8];
      if (!hex16ToBytes(panelMap_[i].longAddr, longAddr)) {
        continue;
      }
      if (count == 0) {
        copyString(firstLongAddr, sizeof(firstLongAddr), panelMap_[i].longAddr);
      }
      memcpy(data + idx, longAddr, sizeof(longAddr));
      idx += sizeof(longAddr);
      data[idx++] = TIGO_NODE_SEED_NODE_ID_HIGH;
      data[idx++] = nodeIdForPanelIndex(panelIndex);
      ++count;
    }
    if (count == 0) {
      return false;
    }
    data[0] = count;
    addEvent("node seed chunk start=%u count=%u body=%u total=%u first=%s node_base=%u node_high=0x%02X",
             (unsigned)startPanelIndex,
             (unsigned)count,
             (unsigned)idx,
             (unsigned)(idx + 5U),
             firstLongAddr,
             (unsigned)TIGO_NODE_ID_BASE,
             (unsigned)TIGO_NODE_SEED_NODE_ID_HIGH);
    bytesToHex(data, idx, lastPvSubcommandRequestHex_, sizeof(lastPvSubcommandRequestHex_));
    return sendPvSubcommand(0x29, data, idx, 0x2A);
  }

  bool setPvConfig(uint16_t nodeId, uint16_t periodSlots, uint16_t phaseSlots, uint8_t reportType = 0x31) {
    if (!beginTapCommand(TapCommandKind::PvConfig, "pv config")) {
      return false;
    }
    static const uint8_t tail[] = {
      0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x30, 0x02, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t data[2 + 2 + 2 + 2 + 2 + sizeof(tail)];
    size_t idx = 0;
    data[idx++] = (uint8_t)(nodeId >> 8);
    data[idx++] = (uint8_t)(nodeId & 0xFF);
    data[idx++] = 0x03;
    data[idx++] = 0x00;
    data[idx++] = reportType;
    data[idx++] = 0x02;
    data[idx++] = (uint8_t)(periodSlots >> 8);
    data[idx++] = (uint8_t)(periodSlots & 0xFF);
    data[idx++] = (uint8_t)(phaseSlots >> 8);
    data[idx++] = (uint8_t)(phaseSlots & 0xFF);
    memcpy(data + idx, tail, sizeof(tail));
    idx += sizeof(tail);

    commandState_.arg0 = nodeId;
    commandState_.arg1 = periodSlots;
    commandState_.arg2 = phaseSlots;
    commandState_.arg3 = reportType;
    commandState_.expectedPvResponseSubcmd = 0x14;
    sendQueuedPvCommand(0x13, data, idx, commandState_.ackHex, sizeof(commandState_.ackHex), "waiting for pv config ack");
    return true;
  }

  bool sendPvSubcommand(uint8_t subcmd, const uint8_t* data, size_t dataLen, uint8_t expectedResponseSubcmd = 0) {
    if (!beginTapCommand(TapCommandKind::PvSubcommand, "pv subcommand")) {
      return false;
    }
    commandState_.arg0 = subcmd;
    commandState_.expectedPvResponseSubcmd = expectedResponseSubcmd;
    lastPvSubcommandRequestLen_ = dataLen;
    lastPvSubcommandRequestTruncated_ = (dataLen * 2U + 1U) > sizeof(lastPvSubcommandRequestHex_);
    bytesToHex(data, dataLen, lastPvSubcommandRequestHex_, sizeof(lastPvSubcommandRequestHex_));
    lastPvSubcommandAckHex_[0] = '\0';
    lastPvAckBodyHex_[0] = '\0';
    lastPvAckStatusFlags_ = 0;
    lastPvAckResponseSubcmd_ = 0;
    sendQueuedPvCommand(subcmd, data, dataLen, commandState_.ackHex, sizeof(commandState_.ackHex), "waiting for pv subcommand ack");
    return true;
  }

  bool sendNodeTextCommand(uint16_t nodeId, const char* text, bool appendCr = true) {
    if (text == nullptr || text[0] == '\0') {
      return false;
    }
    uint8_t data[64];
    size_t len = 0;
    data[len++] = (uint8_t)(nodeId >> 8);
    data[len++] = (uint8_t)(nodeId & 0xFF);
    while (*text && len < sizeof(data)) {
      data[len++] = (uint8_t)*text++;
    }
    if (appendCr && len < sizeof(data) && data[len - 1] != '\r') {
      data[len++] = '\r';
    }
    if (text != nullptr && *text != '\0') {
      return false;
    }
    return sendPvSubcommand(0x06, data, len, 0x07);
  }

  bool sendNodeOpcodeCommand(uint8_t subcmd, uint16_t nodeId, uint8_t opcode) {
    uint8_t data[3];
    data[0] = (uint8_t)(nodeId >> 8);
    data[1] = (uint8_t)(nodeId & 0xFF);
    data[2] = opcode;
    const uint8_t expectedResponseSubcmd = subcmd == 0x17 ? 0x18 : 0;
    return sendPvSubcommand(subcmd, data, sizeof(data), expectedResponseSubcmd);
  }

  bool sendNodeExtendedSmrtCommand(uint16_t nodeId) {
    return sendNodeTextCommand(nodeId, "^00Smrt_........S0000...0000", true);
  }

  bool setActivePollingEnabled(bool enabled, const char* source) {
    if (!enabled) {
      enterPassiveListenOnly(source);
    }
    if (activePollingEnabled_ == enabled) {
      return false;
    }
    activePollingEnabled_ = enabled;
    if (enabled) {
      lastPollSentMs_ = platformMillis() - TIGO_RS485_POLL_INTERVAL_MS;
      const bool networkNeedsConfirmation =
          lastNetworkStatusValid_ && lastNetworkExpectedNodes_ > 0 &&
          lastNetworkConfirmedNodes_ < lastNetworkExpectedNodes_ &&
          countValidNodeMap() > 0;
      if (networkNeedsConfirmation) {
        nodeWakeCompleted_ = false;
        lastNodeWakeAttemptMs_ = 0;
        addEvent("polling resume queued incomplete network recovery: %u/%u",
                 lastNetworkConfirmedNodes_, lastNetworkExpectedNodes_);
      }
    }
    statusDirty_ = true;
    addEvent("polling %s via %s", enabled ? "enabled" : "disabled", source ? source : "local");
    if (!savePollingSettingsToPersistentStore()) {
      addEvent("polling setting save failed");
    }
    return true;
  }

  bool enumerateGateway(uint16_t enumId, uint16_t desiredGatewayId) {
    if (!beginTapCommand(TapCommandKind::Enumerate, "enumeration")) {
      return false;
    }
    const uint16_t beforeId = bootJournal_.lastWorkingGatewayId != 0
        ? bootJournal_.lastWorkingGatewayId : gatewayId_;
    beginAddressTransaction(0x003C, beforeId, desiredGatewayId,
                            "enumeration/address transaction");
    recordMutationRequested(0x0014, 0, desiredGatewayId,
                            "explicit TAP enumeration");
    if (!flushBootJournal(true)) {
      failAddressTransaction();
      failTapCommand("enumeration journal flush failed before transmit");
      return false;
    }
    commandState_.arg0 = enumId;
    commandState_.arg1 = desiredGatewayId;
    commandState_.attempt = 0;
    commandState_.nextActionMs = 0;
    copyString(commandState_.status, sizeof(commandState_.status), "enumeration start burst");
    commandState_.phase = TapCommandPhase::EnumerateStartBurst;
    return true;
  }

  bool sendSimpleFrameCommandTo(uint16_t targetGatewayId,
                                 uint16_t typeCode,
                                 const uint8_t* payload,
                                 size_t payloadLen,
                                 uint16_t expectedType,
                                 uint16_t expectedGatewayId,
                                 const char* status,
                                 uint32_t timeoutMs = 800,
                                 TapCommandKind commandKind = TapCommandKind::SimpleFrame) {
    if (!beginTapCommand(commandKind, status)) {
      return false;
    }
    const bool rsdControl = typeCode == 0x0B00 && payload != nullptr &&
                            payloadLen == 1 && payload[0] <= 0x01;
    commandState_.arg0 = typeCode;
    commandState_.arg1 = targetGatewayId;
    commandState_.arg2 = expectedGatewayId;
    commandState_.arg3 = rsdControl ? payload[0] : 0xFF;
    if (rsdControl) {
      ++destructiveManagementFramesTx_;
      recordMutationRequested(0x0B00, payload[0], targetGatewayId,
                              payload[0] == 0x01
                                  ? "RSD RUN requested"
                                  : "RSD STOP requested");
      if (!flushBootJournal(true)) {
        failTapCommand("RSD control journal flush failed before transmit");
        return false;
      }
    }
    if (bootCcaStep_ != UINT8_MAX && status != nullptr && strncmp(status, "cca ", 4) == 0) {
      addEvent("cca boot step %u tx target=%04X type=%04X expect=%04X/%04X",
               (unsigned)bootCcaStep_,
               targetGatewayId,
               typeCode,
               expectedGatewayId,
               expectedType);
    }
    if (!sendGatewayFrame(targetGatewayId, typeCode, payload, payloadLen)) {
      failTapCommand("frame blocked before transmit");
      return false;
    }
    setTapCommandWait(TapCommandPhase::WaitingFrame, expectedType, expectedGatewayId, timeoutMs, status);
    return true;
  }

  bool sendSimpleFrameCommand(uint16_t typeCode,
                              const uint8_t* payload,
                              size_t payloadLen,
                              uint16_t expectedType,
                              const char* status,
                              uint32_t timeoutMs = 800) {
    return sendSimpleFrameCommandTo(gatewayId_, typeCode, payload, payloadLen, expectedType, gatewayId_, status, timeoutMs);
  }

  bool sendSimpleFrameNow(uint16_t targetGatewayId, uint16_t typeCode, const uint8_t* payload, size_t payloadLen) {
    if (tapCommandActive() || awaitingReceiveResponse_) {
      return false;
    }
    if (!sendGatewayFrame(targetGatewayId, typeCode, payload, payloadLen)) {
      return false;
    }
    addEvent("simple frame sent target=%04X type=%04X len=%u",
             targetGatewayId,
             typeCode,
             (unsigned)payloadLen);
    return true;
  }

  bool sendBootReceiveSeed(const uint8_t* payload, size_t payloadLen, const char* status) {
    if (!beginTapCommand(TapCommandKind::BootReceiveSeed, status)) {
      return false;
    }
    commandState_.arg0 = payloadLen >= 4
        ? (uint16_t)(((uint16_t)payload[2] << 8) | payload[3]) : 0xFFFFU;
    cursorEpochResetPending_ = payload != nullptr && payloadLen == 5 &&
                               memcmp(payload, "\x00\x00\x00\x00\x00", 5) == 0;
    if (!sendGatewayFrame(gatewayId_, 0x0148, payload, payloadLen)) {
      failTapCommand("receive bootstrap blocked before transmit");
      return false;
    }
    setTapCommandWait(TapCommandPhase::WaitingFrame, 0x0149, gatewayId_, 800, status);
    return true;
  }

  bool sendMagicHandshakeFrame(uint16_t typeCode, uint16_t expectedType,
                               const char* status,
                               uint16_t targetGatewayId = 0xFFFFU,
                               bool verifyAfterAck = true) {
    if (!beginTapCommand(TapCommandKind::SimpleFrame, status)) {
      return false;
    }
    static const uint8_t magic[] = {0x37, 0x24, 0x92, 0x66};
    const uint16_t target = targetGatewayId == 0xFFFFU ? gatewayId_ : targetGatewayId;
    const uint16_t lastVerifiedId = bootJournal_.lastWorkingGatewayId != 0
        ? bootJournal_.lastWorkingGatewayId : gatewayId_;
    commandState_.arg0 = typeCode;
    commandState_.arg1 = target;
    commandState_.arg2 = gatewayId_;
    commandState_.arg3 = verifyAfterAck ? 1 : 0;
    if (typeCode == 0x0012 || typeCode == 0x0010) {
      beginAddressTransaction(typeCode, lastVerifiedId, gatewayId_,
                              typeCode == 0x0012
                                  ? "STORE operational address"
                                  : "APPLY operational address");
      recordMutationRequested(typeCode, 0, gatewayId_,
                              typeCode == 0x0012 ? "STORE operational address" : "APPLY operational address");
      if (!flushBootJournal(true)) {
        failTapCommand("address journal flush failed before transmit");
        failAddressTransaction();
        return false;
      }
    }
    if (!sendGatewayFrame(target, typeCode, magic, sizeof(magic))) {
      failTapCommand("address handshake blocked before transmit");
      failAddressTransaction();
      return false;
    }
    setTapCommandWait(TapCommandPhase::WaitingFrame, expectedType, target, TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS, status);
    return true;
  }

  bool resolveTapLongAddress(uint8_t* longAddr, char* source, size_t sourceLen) {
    if (longAddr == nullptr) {
      return false;
    }
    if (hex16ToBytes(gatewayLongAddr_, longAddr)) {
      copyString(source, sourceLen, "learned");
      return true;
    }
    if (hex16ToBytes(TIGO_TAP_LONG_ADDR_HEX, longAddr)) {
      bytesToHex(longAddr, 8, gatewayLongAddr_, sizeof(gatewayLongAddr_));
      copyString(source, sourceLen, "config");
      markPersistentStateDirty();
      return true;
    }
    copyString(source, sourceLen, "missing");
    return false;
  }

  bool tapLongAddressResolvable() const {
    uint8_t ignored[8];
    return hex16ToBytes(gatewayLongAddr_, ignored) || hex16ToBytes(TIGO_TAP_LONG_ADDR_HEX, ignored);
  }

  bool sendGatewayAssignFrameCommand(uint16_t targetGatewayId, uint16_t desiredGatewayId, const char* status) {
    if (tapCommandActive() || awaitingReceiveResponse_) {
      return false;
    }
    uint8_t longAddr[8];
    char source[12];
    if (!resolveTapLongAddress(longAddr, source, sizeof(source))) {
      addEvent("assign frame missing tap long address; enumerate or set TIGO_TAP_LONG_ADDR_HEX");
      return false;
    }
    static const uint8_t enumMagic[] = {0x37, 0x24, 0x92, 0x66};
    uint8_t payload[14];
    memcpy(payload, enumMagic, sizeof(enumMagic));
    memcpy(payload + 4, longAddr, sizeof(longAddr));
    payload[12] = (uint8_t)(desiredGatewayId >> 8);
    payload[13] = (uint8_t)(desiredGatewayId & 0xFF);
    addEvent("assign frame target=%04X desired=%04X tap=%s source=%s",
             targetGatewayId,
             desiredGatewayId,
             gatewayLongAddr_,
             source);
    // Persist both the address transaction and the requested destination
    // before the first byte can reach the TAP.
    beginAddressTransaction(0x003C, targetGatewayId, desiredGatewayId,
                            "gateway address assignment");
    recordMutationRequested(0x003C, 0, desiredGatewayId,
                            "gateway address assignment");
    if (!flushBootJournal(true)) {
      finishMutationJournal(false);
      failAddressTransaction();
      addEvent("address assignment blocked because journal flush failed");
      return false;
    }
    const bool started = sendSimpleFrameCommandTo(targetGatewayId,
                                                  0x003C,
                                                  payload,
                                                  sizeof(payload),
                                                  0x003D,
                                                  targetGatewayId,
                                                  status,
                                                  TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
    if (!started) {
      finishMutationJournal(false);
      failAddressTransaction();
    }
    return started;
  }

  bool sendEnumStartFrame(const char* status) {
    static const uint8_t payload[] = {
      0x37, 0x24, 0x92, 0x66,
      (uint8_t)(TIGO_ENUM_ID >> 8),
      (uint8_t)(TIGO_ENUM_ID & 0xFF)
    };
    return sendSimpleFrameCommandTo(0x0000,
                                    0x0014,
                                    payload,
                                    sizeof(payload),
                                    0x0015,
                                    0x0000,
                                    status,
                                    TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
  }

  bool sendRsdControlFrame(bool run, const char* status,
                           TapCommandKind commandKind = TapCommandKind::SimpleFrame) {
    const uint8_t payload[] = {run ? (uint8_t)0x01 : (uint8_t)0x00};
    return sendSimpleFrameCommandTo(gatewayId_,
                                    0x0B00,
                                    payload,
                                    sizeof(payload),
                                    0x0B01,
                                    gatewayId_,
                                    status,
                                    TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                                    commandKind);
  }

 private:
  // --------------------------
  // Timing helpers
  // --------------------------
  static uint32_t elapsed(uint32_t now, uint32_t since) {
    return now - since;
  }

  static bool timeReached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
  }

  static bool timeBefore(uint32_t now, uint32_t target) {
    return !timeReached(now, target);
  }

  bool ntpTimeValid() const {
    return time(nullptr) > 1700000000;
  }

  void sanitizeInlineText(char* text) const {
    if (text == nullptr) {
      return;
    }
    while (*text) {
      if (*text == '\r' || *text == '\n') {
        *text = ' ';
      }
      ++text;
    }
  }

  void formatClockFromSeconds(uint32_t totalSeconds, char* out, size_t outLen) const {
    const uint32_t sec = totalSeconds % 60UL;
    const uint32_t min = (totalSeconds / 60UL) % 60UL;
    const uint32_t hour = (totalSeconds / 3600UL) % 24UL;
    snprintf(out, outLen, "%02lu:%02lu:%02lu",
             (unsigned long)hour,
             (unsigned long)min,
             (unsigned long)sec);
    if (outLen > 0) {
      out[outLen - 1] = '\0';
    }
  }

  void formatFrameTimestamp(uint32_t now, uint32_t frameMs, char* out, size_t outLen) const {
    const uint32_t ageMs = elapsed(now, frameMs);
    if (ntpTimeValid()) {
      time_t ts = time(nullptr) - (time_t)(ageMs / 1000UL);
      struct tm info;
      localtime_r(&ts, &info);
      snprintf(out, outLen, "%02d:%02d:%02d", info.tm_hour, info.tm_min, info.tm_sec);
      if (outLen > 0) {
        out[outLen - 1] = '\0';
      }
      return;
    }
    formatClockFromSeconds(frameMs / 1000UL, out, outLen);
  }

  void fillCurrentIp(char* out, size_t outLen) const {
    platformRuntime_.fillIpAddress(apMode_, out, outLen);
  }

  uint32_t platformMillis() const {
    return platformRuntime_.millis32();
  }

  void platformYield() const {
    platformRuntime_.yieldNow();
  }

  void platformDelayMilliseconds(uint32_t ms) const {
    platformRuntime_.delayMilliseconds(ms);
  }

  void platformDelayMicroseconds(uint32_t us) const {
    platformRuntime_.delayMicrosecondsExact(us);
  }

  const char* tapCommandName(TapCommandKind kind) const {
    switch (kind) {
      case TapCommandKind::Ping: return "ping";
      case TapCommandKind::RsdControl: return "rsd_control";
      case TapCommandKind::Version: return "version";
      case TapCommandKind::NodeTable: return "node_table";
      case TapCommandKind::NetworkStatus: return "network_status";
      case TapCommandKind::RadioConfig: return "radio_config";
      case TapCommandKind::PvConfig: return "pv_config";
      case TapCommandKind::PvSubcommand: return "pv_subcmd";
      case TapCommandKind::Enumerate: return "enumerate";
      case TapCommandKind::SimpleFrame: return "simple_frame";
      case TapCommandKind::BootReceiveSeed: return "boot_rx_seed";
      case TapCommandKind::None:
      default: return "idle";
    }
  }

  bool tapCommandActive() const {
    return commandState_.kind != TapCommandKind::None;
  }

  uint8_t nodeIdForPanelIndex(uint16_t panelIndex) const {
    const uint16_t nodeId = (uint16_t)(TIGO_NODE_ID_BASE + panelIndex);
    return nodeId > 0xFFU ? 0xFFU : (uint8_t)nodeId;
  }

  uint16_t normalizeNodeId(uint16_t nodeId) const {
    return nodeId & 0x7FFFU;
  }

  const char* nodeSeedStateName() const {
    switch (nodeSeedState_) {
      case NodeSeedState::Idle: return "idle";
      case NodeSeedState::ClearTable: return "clear_table";
      case NodeSeedState::SendChunk: return "send_chunk";
      case NodeSeedState::VerifyNodeTable: return "verify_node_table";
      case NodeSeedState::StartLearn: return "start_learn";
      case NodeSeedState::EnablePv: return "enable_pv";
      case NodeSeedState::Done: return "done";
      case NodeSeedState::Failed: return "failed";
      default: return "unknown";
    }
  }

  uint16_t countPanelMapLongAddrs() const {
    uint16_t count = 0;
    const uint16_t maxPanels = TIGO_MAX_OPTIMIZERS;
    for (size_t i = 0; i < TIGO_MAX_OPTIMIZERS && count < maxPanels; ++i) {
      if (panelMap_[i].longAddr[0] != '\0') {
        ++count;
      }
    }
    return count;
  }

  void abortTapCommand(const char* message) {
    if (!tapCommandActive()) {
      return;
    }
    finishMutationJournal(false);
    failAddressTransaction();
    copyString(lastCommandName_, sizeof(lastCommandName_), tapCommandName(commandState_.kind));
    copyString(lastCommandMessage_, sizeof(lastCommandMessage_), message ? message : "aborted");
    lastCommandOk_ = false;
    lastCommandCompletedMs_ = platformMillis();
    ++lastCommandCompletionGeneration_;
    memset(&commandState_, 0, sizeof(commandState_));
    commandState_.kind = TapCommandKind::None;
    commandState_.phase = TapCommandPhase::Idle;
    statusDirty_ = true;
    addEvent("%s aborted: %s", lastCommandName_, lastCommandMessage_);
  }

  void enterPassiveListenOnly(const char* source) {
    awaitingReceiveResponse_ = false;
    abortTapCommand("polling disabled");
    if (nodeWakeActive_) {
      finishNodeWakeSequence(false, "polling disabled");
    }
    nodeWakeCompleted_ = true;
    bootCcaStep_ = UINT8_MAX;
    bootCcaWaitingStep_ = UINT8_MAX;
    bootCcaRetryCount_ = 0;
    bootEnumeratePending_ = false;
    bootVersionPending_ = false;
    bootRadioConfigPending_ = false;
    bootGatewaySelector0Pending_ = false;
    bootNetworkStatusPending_ = false;
    bootNodeTablePending_ = false;
    bootGatewaySelector1Pending_ = false;
    bootNodeTableEndPending_ = false;
    statusDirty_ = true;
    addEvent("passive listen-only via %s", source ? source : "local");
  }

  bool beginTapCommand(TapCommandKind kind, const char* status) {
    if (gatewayId_ == 0 && kind != TapCommandKind::Enumerate) {
      return false;
    }
    if (tapCommandActive() || awaitingReceiveResponse_) {
      return false;
    }
    memset(&commandState_, 0, sizeof(commandState_));
    lastCommandResponseType_ = 0;
    lastCommandResponseGatewayId_ = 0;
    lastCommandResponseDsn_ = 0;
    commandState_.kind = kind;
    commandState_.phase = TapCommandPhase::Idle;
    copyString(commandState_.status, sizeof(commandState_.status), status);
    statusDirty_ = true;
    return true;
  }

  void setTapCommandWait(TapCommandPhase phase, uint16_t typeCode, uint16_t gatewayId, uint32_t timeoutMs, const char* status) {
    commandState_.phase = phase;
    commandState_.expectedType = typeCode;
    commandState_.expectedGatewayId = gatewayId;
    commandState_.deadlineMs = platformMillis() + timeoutMs;
    copyString(commandState_.status, sizeof(commandState_.status), status);
    statusDirty_ = true;
  }

  void finishTapCommand(bool ok, const char* message) {
    const TapCommandKind finishedKind = commandState_.kind;
    const uint16_t finishedArg0 = commandState_.arg0;
    const bool ccaManagedCommand = bootCcaWaitingStep_ != UINT8_MAX;
    const bool unresolvedAddressTransaction =
        bootJournal_.rollbackNeeded &&
        bootJournal_.transactionState != (uint8_t)AddressTransactionState::Verified;
    const bool mutationPending = activeMutationIndex_ >= 0;
    finishMutationJournal(ok);
    const bool mutationResultStored = !mutationPending || flushBootJournal(true);
    if (!mutationResultStored) {
      ok = false;
      message = "mutation result journal flush failed";
    }
    if (finishedKind == TapCommandKind::BootReceiveSeed && !ok) {
      cursorEpochResetPending_ = false;
    }
    if (!ok && unresolvedAddressTransaction) {
      failAddressTransaction();
    }
    copyString(lastCommandName_, sizeof(lastCommandName_), tapCommandName(commandState_.kind));
    copyString(lastCommandMessage_, sizeof(lastCommandMessage_), message ? message : (ok ? "ok" : "failed"));
    statusDirty_ = true;
    if (ok) {
      addEvent("%s ok", lastCommandName_);
    } else {
      addEvent("%s failed: %s", lastCommandName_, lastCommandMessage_);
    }
    lastCommandOk_ = ok;
    lastCommandCompletedMs_ = platformMillis();
    ++lastCommandCompletionGeneration_;
    memset(&commandState_, 0, sizeof(commandState_));
    commandState_.kind = TapCommandKind::None;
    commandState_.phase = TapCommandPhase::Idle;
    const bool warmAttachHandled = handleReadOnlyWarmAttachCommandDone(finishedKind, ok);
    if (!warmAttachHandled) {
      handleCcaCompatBootCommandDone(finishedKind, ok);
    }
    handleNodeSeedCommandDone(finishedKind, finishedArg0, ok);
    handleRfNodePromotionCommandDone(finishedKind, finishedArg0, ok);
    if (!ok && unresolvedAddressTransaction && !warmAttachHandled &&
        !ccaManagedCommand && !traceReplayActive() &&
        (warmAttachPhase_ == WarmAttachPhase::Idle ||
         warmAttachPhase_ == WarmAttachPhase::Complete ||
         warmAttachPhase_ == WarmAttachPhase::Failed)) {
      addEvent("address-changing command failed; starting non-destructive candidate-ID recovery");
      beginReadOnlyWarmAttach(true);
    }
  }

  void failTapCommand(const char* message) {
    finishTapCommand(false, message);
  }

  void sendQueuedPvCommand(uint8_t pvPacketType, const uint8_t* data, size_t dataLen, char* ackHexOut, size_t ackHexOutLen, const char* status) {
    uint8_t payload[TIGO_MAX_PV_COMMAND_PAYLOAD];
    constexpr size_t envelopeLen = 5;
    if (dataLen + envelopeLen > sizeof(payload)) {
      addEvent("pv command too large: subcmd=0x%02X data=%u total=%u max=%u",
               (unsigned)pvPacketType,
               (unsigned)dataLen,
               (unsigned)(dataLen + envelopeLen),
               (unsigned)sizeof(payload));
      failTapCommand("pv command too large");
      return;
    }
    const uint8_t seq = nextSequence();
    payload[0] = 0x00;
    payload[1] = 0x00;
    payload[2] = 0x00;
    payload[3] = pvPacketType;
    payload[4] = seq;
    commandState_.expectedDsn = seq;
    if (dataLen > 0 && data != nullptr) {
      memcpy(payload + envelopeLen, data, dataLen);
    }
    if (TIGO_PV_COMMAND_TX_DIAG_LOG &&
        (pvPacketType == 0x06 || pvPacketType == 0x17 || pvPacketType == 0x13)) {
      char txHex[192];
      bytesToHex(payload, dataLen + envelopeLen, txHex, sizeof(txHex));
      addEvent("pv-subcmd 0x%02X tx=%s", (unsigned)pvPacketType, txHex);
    }
    if (ackHexOut && ackHexOutLen > 0) {
      ackHexOut[0] = '\0';
    }
    const bool radioWrite = pvPacketType == 0x0D && dataLen > 2;
    if (pvPacketType == 0x06 || pvPacketType == 0x17) {
      ++activeRfFramesTx_;
    }
    const bool stateChanging =
        radioWrite || pvPacketType == 0x13 || pvPacketType == 0x22 ||
        pvPacketType == 0x29 || pvPacketType == 0x2B ||
        pvPacketType == 0x2D || pvPacketType == 0x41;
    if (stateChanging) {
      ++destructiveManagementFramesTx_;
      char label[32];
      snprintf(label, sizeof(label), "PV subcommand 0x%02X", (unsigned)pvPacketType);
      recordMutationRequested(0x0B0F, pvPacketType, gatewayId_, label);
      if (!flushBootJournal(true)) {
        failTapCommand("mutation journal flush failed before transmit");
        return;
      }
    }
    if (!sendGatewayFrame(gatewayId_, 0x0B0F, payload, dataLen + envelopeLen)) {
      failTapCommand("PV command blocked before transmit");
      return;
    }
    setTapCommandWait(TapCommandPhase::WaitingFrame, 0x0B10, gatewayId_, TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS, status);
  }

  void addWarmAttachCandidate(uint16_t candidate) {
    if (candidate == 0) {
      return;
    }
    for (uint8_t i = 0; i < warmAttachCandidateCount_; ++i) {
      if (warmAttachCandidates_[i] == candidate) {
        return;
      }
    }
    if (warmAttachCandidateCount_ < sizeof(warmAttachCandidates_) / sizeof(warmAttachCandidates_[0])) {
      warmAttachCandidates_[warmAttachCandidateCount_++] = candidate;
    }
  }

  void buildWarmAttachCandidates() {
    warmAttachCandidateCount_ = 0;
    warmAttachCandidateIndex_ = 0;
    if (bootJournal_.rollbackNeeded) {
      addWarmAttachCandidate(bootJournal_.transactionBeforeId);
      addWarmAttachCandidate(bootJournal_.transactionRequestedId);
    }
    addWarmAttachCandidate(bootJournal_.lastWorkingGatewayId);
    addWarmAttachCandidate(persistedGatewayId_);
    addWarmAttachCandidate(gatewayId_);
    addWarmAttachCandidate(TIGO_DESIRED_GATEWAY_ID);
    addWarmAttachCandidate(0x120A);
    addWarmAttachCandidate(TIGO_ENUM_ID);
  }

  void beginReadOnlyWarmAttach(bool recoverySearch) {
    finishNodeWakeSequence(false, recoverySearch ? "address recovery search" : "read-only warm attach");
    readOnlyWarmAttachProtectsState_ = true;
    recoveryAuthorized_ = false;
    warmAttachRecoverySearch_ = recoverySearch;
    warmAttachCommandPending_ = false;
    warmAttachProbeId_ = 0;
    buildWarmAttachCandidates();
    warmAttachPhase_ = recoverySearch || TIGO_WARM_ATTACH_PASSIVE_LISTEN_MS == 0
        ? WarmAttachPhase::ProbeCandidate
        : WarmAttachPhase::PassiveListen;
    warmAttachNextActionMs_ = platformMillis() +
        (warmAttachPhase_ == WarmAttachPhase::PassiveListen
             ? TIGO_WARM_ATTACH_PASSIVE_LISTEN_MS : 0);
    tapBootPath_ = warmAttachPhase_ == WarmAttachPhase::PassiveListen
        ? TapBootPath::PassiveListen : TapBootPath::ReadOnlyWarmAttach;
    bootJournal_.lastBootPath = (uint8_t)tapBootPath_;
    bootJournal_.lastCursorStrategy = (uint8_t)bootCursorStrategy_;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
               recoverySearch ? "candidate-id recovery" : "read-only warm attach");
    markBootJournalDirty();
    addEvent("read-only warm attach: passive=%lums cursor=%s candidates=%u%s",
             (unsigned long)(recoverySearch ? 0 : TIGO_WARM_ATTACH_PASSIVE_LISTEN_MS),
             bootCursorStrategyName(bootCursorStrategy_),
             (unsigned)warmAttachCandidateCount_,
             recoverySearch ? " recovery" : "");
  }

  bool warmAttachTrafficGateActive() const {
    return warmAttachPhase_ != WarmAttachPhase::Idle &&
           warmAttachPhase_ != WarmAttachPhase::Complete;
  }

  void completeReadOnlyWarmAttach() {
    warmAttachPhase_ = WarmAttachPhase::Complete;
    tapBootPath_ = TapBootPath::ReadOnlyPolling;
    readOnlyWarmAttachProtectsState_ = true;
    recoveryAuthorized_ = false;
    lastPollSentMs_ = platformMillis() - TIGO_RS485_POLL_INTERVAL_MS;
    if (!cursorStallRecoveryAttempted_) {
      cursorRecoveryPollTimeoutBaseline_ = pollTimeouts_;
    }
    bootJournal_.lastWorkingGatewayId = gatewayId_;
    bootJournal_.lastBootPath = (uint8_t)tapBootPath_;
    bootJournal_.lastTapState = (uint8_t)classifyTapState(platformMillis());
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation), "read-only attach complete");
    markPersistentStateDirty();
    markBootJournalDirty();
    flushBootJournal(true);
    addEvent("read-only warm attach complete gateway=%04X cursor=%04X state=%s; recovery locked",
             gatewayId_, nextPacketNumber_, tapObservedStateName(classifyTapState(platformMillis())));
  }

  void failReadOnlyWarmAttach() {
    warmAttachPhase_ = WarmAttachPhase::Failed;
    tapBootPath_ = TapBootPath::ReadOnlyWarmAttach;
    readOnlyWarmAttachProtectsState_ = true;
    recoveryAuthorized_ = false;
    bootJournal_.lastBootPath = (uint8_t)tapBootPath_;
    bootJournal_.lastTapState = (uint8_t)TapObservedState::UnknownUnreachable;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation), "warm attach unresolved");
    markBootJournalDirty();
    flushBootJournal(true);
    addEvent("read-only warm attach unresolved; polling blocked until verified ID or explicit recovery");
  }

  void processReadOnlyWarmAttach(uint32_t now) {
    if (warmAttachPhase_ == WarmAttachPhase::Idle ||
        warmAttachPhase_ == WarmAttachPhase::Complete ||
        warmAttachPhase_ == WarmAttachPhase::Failed ||
        warmAttachCommandPending_ || timeBefore(now, warmAttachNextActionMs_)) {
      return;
    }
    switch (warmAttachPhase_) {
      case WarmAttachPhase::PassiveListen:
        tapBootPath_ = TapBootPath::ReadOnlyWarmAttach;
        warmAttachPhase_ = WarmAttachPhase::ProbeCandidate;
        addEvent("read-only passive listen complete; probing known IDs");
        return;
      case WarmAttachPhase::ProbeCandidate:
        if (warmAttachCandidateIndex_ >= warmAttachCandidateCount_) {
          failReadOnlyWarmAttach();
          return;
        }
        warmAttachProbeId_ = warmAttachCandidates_[warmAttachCandidateIndex_];
        addEvent("read-only ID probe %u/%u target=%04X",
                 (unsigned)(warmAttachCandidateIndex_ + 1),
                 (unsigned)warmAttachCandidateCount_, warmAttachProbeId_);
        warmAttachCommandPending_ = sendSimpleFrameCommandTo(
            warmAttachProbeId_, 0x003A, nullptr, 0, 0x003B,
            warmAttachProbeId_, "read-only network info", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
        return;
      case WarmAttachPhase::FactoryAssign:
        {
        const uint16_t rollbackId = warmAttachRecoveryTargetId_ != 0
            ? warmAttachRecoveryTargetId_ : TIGO_DESIRED_GATEWAY_ID;
        warmAttachCommandPending_ = sendGatewayAssignFrameCommand(
            TIGO_ENUM_ID, rollbackId,
            "targeted rollback address assignment");
        if (!warmAttachCommandPending_) {
          failReadOnlyWarmAttach();
        }
        return;
        }
      case WarmAttachPhase::FactoryVerify:
        {
        const uint16_t rollbackId = warmAttachRecoveryTargetId_ != 0
            ? warmAttachRecoveryTargetId_ : TIGO_DESIRED_GATEWAY_ID;
        warmAttachCommandPending_ = sendSimpleFrameCommandTo(
            rollbackId, 0x003A, nullptr, 0, 0x003B,
            rollbackId, "verify rollback gateway address",
            TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
        if (!warmAttachCommandPending_) {
          failAddressTransaction();
          beginReadOnlyWarmAttach(true);
        }
        return;
        }
      case WarmAttachPhase::Version:
        warmAttachCommandPending_ = requestVersion();
        return;
      case WarmAttachPhase::RadioSelector0:
        warmAttachCommandPending_ = requestGatewayRadioConfig(0x0000);
        return;
      case WarmAttachPhase::RadioSelector1:
        warmAttachCommandPending_ = requestGatewayRadioConfig(0x0001);
        return;
      case WarmAttachPhase::NetworkStatus:
        warmAttachCommandPending_ = requestNetworkStatus();
        return;
      case WarmAttachPhase::NodeTable:
        warmAttachCommandPending_ = requestNodeTable(0);
        return;
      case WarmAttachPhase::CursorBootstrapZero: {
        static const uint8_t payload[] = {0x00, 0x00, 0x00, 0x00, 0x00};
        lastRequestedPacketNumber_ = 0x0000;
        warmAttachCommandPending_ = sendBootReceiveSeed(
            payload, sizeof(payload), "read-only CCA cursor bootstrap 0000");
        return;
      }
      case WarmAttachPhase::CursorBootstrapEeee: {
        static const uint8_t payload[] = {0x00, 0x00, 0xEE, 0xEE, 0x00};
        lastRequestedPacketNumber_ = 0xEEEE;
        warmAttachCommandPending_ = sendBootReceiveSeed(
            payload, sizeof(payload), "read-only CCA cursor bootstrap EEEE");
        return;
      }
      default:
        return;
    }
  }

  bool handleReadOnlyWarmAttachCommandDone(TapCommandKind kind, bool ok) {
    if (!warmAttachCommandPending_) {
      return false;
    }
    warmAttachCommandPending_ = false;
    warmAttachNextActionMs_ = platformMillis() + 100;
    switch (warmAttachPhase_) {
      case WarmAttachPhase::ProbeCandidate:
        if (!ok || kind != TapCommandKind::SimpleFrame) {
          ++warmAttachCandidateIndex_;
          addEvent("read-only ID probe failed target=%04X", warmAttachProbeId_);
          return true;
        }
        gatewayId_ = warmAttachProbeId_;
        if (gatewayId_ == TIGO_ENUM_ID) {
          if (!warmAttachRecoverySearch_ || !bootJournal_.rollbackNeeded) {
            bootJournal_.lastTapState = (uint8_t)TapObservedState::FactoryOrUnaddressed;
            copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
                       "enum ID observed; explicit recovery required");
            markBootJournalDirty();
            addEvent("factory/unaddressed TAP observed on %04X; read-only attach will not assign an address",
                     TIGO_ENUM_ID);
            failReadOnlyWarmAttach();
            return true;
          }
          if (factoryAddressAssignmentAttempted_) {
            addEvent("factory TAP still responds on enum ID after verified assignment attempt; explicit recovery required");
            failReadOnlyWarmAttach();
            return true;
          }
          factoryAddressAssignmentAttempted_ = true;
          tapBootPath_ = TapBootPath::TargetedRecovery;
          bootJournal_.lastBootPath = (uint8_t)tapBootPath_;
          bootJournal_.lastTapState = (uint8_t)TapObservedState::FactoryOrUnaddressed;
          readOnlyWarmAttachProtectsState_ = false;
          recoveryAuthorized_ = true;
          warmAttachPhase_ = WarmAttachPhase::FactoryAssign;
          copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
                     "verified factory address recovery");
          markBootJournalDirty();
          const uint16_t rollbackId = bootJournal_.transactionBeforeId != 0
              ? bootJournal_.transactionBeforeId : TIGO_DESIRED_GATEWAY_ID;
          warmAttachRecoveryTargetId_ = rollbackId;
          addEvent("address transaction recovery found TAP on enum ID %04X; restoring last verified ID %04X",
                   TIGO_ENUM_ID, rollbackId);
          return true;
        }
        bootJournal_.lastWorkingGatewayId = gatewayId_;
        if (bootJournal_.rollbackNeeded) {
          bootJournal_.transactionConfirmedId = gatewayId_;
          bootJournal_.transactionState = (uint8_t)AddressTransactionState::Verified;
          bootJournal_.rollbackNeeded = 0;
        }
        markPersistentStateDirty();
        markBootJournalDirty();
        warmAttachPhase_ = WarmAttachPhase::Version;
        addEvent("read-only ID verified target=%04X", gatewayId_);
        return true;
      case WarmAttachPhase::FactoryAssign:
        if (!ok || kind != TapCommandKind::SimpleFrame) {
          failAddressTransaction();
          beginReadOnlyWarmAttach(true);
          return true;
        }
        acknowledgeAddressTransaction();
        warmAttachPhase_ = WarmAttachPhase::FactoryVerify;
        addEvent("factory address assignment acknowledged; verifying %04X",
                 warmAttachRecoveryTargetId_ != 0
                     ? warmAttachRecoveryTargetId_ : TIGO_DESIRED_GATEWAY_ID);
        return true;
      case WarmAttachPhase::FactoryVerify:
        if (!ok || kind != TapCommandKind::SimpleFrame) {
          failAddressTransaction();
          beginReadOnlyWarmAttach(true);
          return true;
        }
        gatewayId_ = warmAttachRecoveryTargetId_ != 0
            ? warmAttachRecoveryTargetId_ : TIGO_DESIRED_GATEWAY_ID;
        verifyAddressTransaction(gatewayId_);
        markPersistentStateDirty();
        readOnlyWarmAttachProtectsState_ = true;
        recoveryAuthorized_ = false;
        warmAttachPhase_ = WarmAttachPhase::Version;
        addEvent("factory address verified on %04X; returning to read-only discovery",
                 gatewayId_);
        return true;
      case WarmAttachPhase::Version:
        if (!ok) addEvent("read-only version probe unavailable; continuing");
        warmAttachPhase_ = WarmAttachPhase::RadioSelector0;
        return true;
      case WarmAttachPhase::RadioSelector0:
        if (!ok) addEvent("read-only radio selector 0 unavailable; continuing");
        warmAttachPhase_ = WarmAttachPhase::RadioSelector1;
        return true;
      case WarmAttachPhase::RadioSelector1:
        if (!ok) addEvent("read-only radio selector 1 unavailable; continuing");
        warmAttachPhase_ = WarmAttachPhase::NetworkStatus;
        return true;
      case WarmAttachPhase::NetworkStatus:
        if (!ok) addEvent("read-only network status unavailable; continuing");
        warmAttachPhase_ = WarmAttachPhase::NodeTable;
        return true;
      case WarmAttachPhase::NodeTable:
        if (!ok) addEvent("read-only node table unavailable; polling for diagnosis only");
        if (bootCursorStrategy_ == BootCursorStrategy::CcaBootstrap) {
          warmAttachPhase_ = WarmAttachPhase::CursorBootstrapZero;
        } else {
          completeReadOnlyWarmAttach();
        }
        return true;
      case WarmAttachPhase::CursorBootstrapZero:
        if (ok && targetedCursorRecoveryActive_) {
          targetedCursorRecoveryActive_ = false;
          addEvent("targeted cursor-stall recovery succeeded with 0000 bootstrap");
          completeReadOnlyWarmAttach();
        } else {
          if (!ok) addEvent("CCA cursor bootstrap 0000 failed; trying EEEE");
          warmAttachPhase_ = WarmAttachPhase::CursorBootstrapEeee;
        }
        return true;
      case WarmAttachPhase::CursorBootstrapEeee:
        if (!ok) addEvent("CCA cursor bootstrap EEEE failed; continuing with last confirmed cursor");
        targetedCursorRecoveryActive_ = false;
        completeReadOnlyWarmAttach();
        return true;
      default:
        return true;
    }
  }

  void processBootCommands(uint32_t now) {
    if (!activePollingEnabled_ || replayExclusiveMode_) {
      return;
    }
    if (tapCommandActive() || awaitingReceiveResponse_) {
      return;
    }
    if (warmAttachPhase_ != WarmAttachPhase::Idle &&
        warmAttachPhase_ != WarmAttachPhase::Complete &&
        warmAttachPhase_ != WarmAttachPhase::Failed) {
      processReadOnlyWarmAttach(now);
      return;
    }
    if (warmAttachPhase_ == WarmAttachPhase::Failed) {
      return;
    }
    if (warmAttachPhase_ == WarmAttachPhase::Complete && readOnlyWarmAttachProtectsState_) {
      const uint32_t cursorTimeouts = pollTimeouts_ - cursorRecoveryPollTimeoutBaseline_;
      if (!cursorStallRecoveryAttempted_ && firstReceiveResponseMs_ == 0 &&
          cursorTimeouts >= TIGO_CURSOR_STALL_RECOVERY_TIMEOUTS) {
        cursorStallRecoveryAttempted_ = true;
        targetedCursorRecoveryActive_ = true;
        warmAttachPhase_ = WarmAttachPhase::CursorBootstrapZero;
        warmAttachNextActionMs_ = now;
        tapBootPath_ = TapBootPath::TargetedRecovery;
        bootJournal_.lastBootPath = (uint8_t)tapBootPath_;
        copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
                   "targeted cursor-stall recovery");
        markBootJournalDirty();
        addEvent("diagnosed receive stall after %lu timeouts; trying minimal 0000 bootstrap",
                 (unsigned long)cursorTimeouts);
      }
      return;
    }
    if (bootEnumeratePending_) {
      if (enumerateGateway(TIGO_ENUM_ID, TIGO_DESIRED_GATEWAY_ID)) {
        bootEnumeratePending_ = false;
        bootEnumerateTried_ = true;
      }
      return;
    }
    if (gatewayId_ == 0) {
      return;
    }
    if (TIGO_CCA_COMPAT_BOOT_SEQUENCE && bootCcaStep_ != UINT8_MAX) {
      processCcaCompatBootStep();
      return;
    }
    if (!bootEnumerateTried_ && tapResponsesRx_ == 0 && pollTimeouts_ >= 20) {
      if (enumerateGateway(TIGO_ENUM_ID, TIGO_DESIRED_GATEWAY_ID)) {
        bootEnumerateTried_ = true;
        addEvent("auto enumeration started after %lu poll timeouts", (unsigned long)pollTimeouts_);
      }
      return;
    }
    if (bootVersionPending_) {
      if (requestVersion()) {
        bootVersionPending_ = false;
      }
      return;
    }
    if (bootRadioConfigPending_) {
      if (requestGatewayRadioConfig()) {
        bootRadioConfigPending_ = false;
      }
      return;
    }
    if (bootGatewaySelector0Pending_) {
      if (requestPvRunState(true)) {
        bootGatewaySelector0Pending_ = false;
      }
      return;
    }
    if (bootNetworkStatusPending_) {
      if (requestNetworkStatus()) {
        bootNetworkStatusPending_ = false;
      }
      return;
    }
    if (bootNodeTablePending_) {
      if (requestNodeTable(0)) {
        bootNodeTablePending_ = false;
      }
      return;
    }
    if (bootGatewaySelector1Pending_) {
      if (requestPvBroadcastEmpty()) {
        bootGatewaySelector1Pending_ = false;
      }
      return;
    }
    if (bootNodeTableEndPending_) {
      if (requestNodeTable(12)) {
        bootNodeTableEndPending_ = false;
      }
    }
  }

  void processCcaCompatBootStep() {
    if (bootCcaWaitingStep_ != UINT8_MAX) {
      return;
    }
    const uint32_t now = platformMillis();
    if (timeBefore(now, bootCcaNextActionMs_)) {
      return;
    }
    switch (bootCcaStep_) {
      case 0:
        if (TIGO_CCA_PREFER_WARM_ATTACH && !bootCcaWarmAttachTried_) {
          bootCcaWarmAttachTried_ = true;
          bootCcaWarmAttachPending_ = true;
          addEvent("cca warm attach probe target=%04X", TIGO_DESIRED_GATEWAY_ID);
          startCcaCompatBootCommand(sendSimpleFrameCommandTo(
              TIGO_DESIRED_GATEWAY_ID, 0x003A, nullptr, 0, 0x003B,
              TIGO_DESIRED_GATEWAY_ID, "cca warm attach",
              TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
          return;
        }
        startCcaCompatBootCommand(sendEnumStartFrame("cca enum start"));
        return;
      case 1:
      case 2:
      case 3:
      case 4:
        startCcaCompatBootCommand(sendEnumStartFrame("cca enum start"));
        return;
      case 5:
        startCcaCompatBootCommand(sendSimpleFrameCommandTo(TIGO_ENUM_ID, 0x0038, nullptr, 0, 0x0039, TIGO_ENUM_ID,
                                                           "cca enum info", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
        return;
      case 6:
        if (!tapLongAddressResolvable()) {
          addEvent("cca assign needs tap long address; falling back to enumeration");
          startCcaCompatBootCommand(enumerateGateway(TIGO_ENUM_ID, TIGO_DESIRED_GATEWAY_ID));
        } else {
          startCcaCompatBootCommand(sendGatewayAssignFrameCommand(TIGO_ENUM_ID, TIGO_DESIRED_GATEWAY_ID, "cca assign desired"));
        }
        return;
      case 7:
        startCcaCompatBootCommand(sendSimpleFrameCommandTo(gatewayId_, 0x003A, nullptr, 0, 0x003B, gatewayId_,
                                                           "cca addr network info", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
        return;
      case 8:
        // The factory CCA stores the operational address before its temporary
        // 0x120A transaction. Omitting this step correlates with TS4 shutdown.
        startCcaCompatBootCommand(sendMagicHandshakeFrame(0x0012, 0x0013, "cca addr store", gatewayId_));
        return;
      case 9:
        if (!shouldRunLateAddressDance()) {
          addEvent("cca late address dance skipped; staying on gateway=%04X", gatewayId_);
          bootCcaStep_ = 13;
          bootCcaRetryCount_ = 0;
          bootCcaNextActionMs_ = now + 100;
          return;
        }
        startCcaCompatBootCommand(sendGatewayAssignFrameCommand(gatewayId_, 0x120A, "cca assign 120a"));
        return;
      case 10:
        startCcaCompatBootCommand(sendSimpleFrameCommandTo(0x120A, 0x003A, nullptr, 0, 0x003B, 0x120A,
                                                           "cca addr network info 120a", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
        return;
      case 11:
        startCcaCompatBootCommand(sendMagicHandshakeFrame(0x0010, 0x0011, "cca addr tap start", 0x0000));
        return;
      case 12:
        ++bootCcaStep_;
        bootCcaNextActionMs_ = now + TIGO_CCA_ADDRESS_DANCE_GAP_MS;
        return;
      case 13:
        startCcaCompatBootCommand(sendSimpleFrameCommandTo(gatewayId_, 0x003A, nullptr, 0, 0x003B, gatewayId_,
                                                           "cca final network info",
                                                           TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
        return;
      case 14:
        startCcaCompatBootCommand(requestVersion());
        return;
      case 15:
        startCcaCompatBootCommand(sendSimpleFrameCommand(0x0E02, nullptr, 0, 0x0006, "cca final e02 probe",
                                                         TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
        return;
      case 16:
        startCcaCompatBootCommand(sendSimpleFrameCommand(0x000E, nullptr, 0, 0x000F, "cca final status probe",
                                                         TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS));
        return;
      case 17:
        {
          static const uint8_t seedPayload[] = {0x00, 0x00, 0x00, 0x00, 0x00};
          startCcaCompatBootCommand(sendBootReceiveSeed(seedPayload, sizeof(seedPayload), "cca zero receive seed"));
        }
        return;
      case 18:
        startCcaCompatBootCommand(sendRsdControlFrame(false, "cca RSD stop"));
        return;
      case 19:
        {
          static const uint8_t seedPayload[] = {0x00, 0x00, 0xEE, 0xEE, 0x00};
          startCcaCompatBootCommand(sendBootReceiveSeed(seedPayload, sizeof(seedPayload), "cca eeee receive seed"));
        }
        return;
      case 20:
        startCcaCompatBootCommand(sendRsdControlFrame(true, "cca RSD run"));
        return;
      case 21:
        startCcaCompatBootCommand(requestGatewayRadioConfig(0x0000));
        return;
      case 22:
        startCcaCompatBootCommand(requestGatewayRadioConfig(0x0001));
        return;
      case 23:
        startCcaCompatBootCommand(requestPvRunState(true));
        return;
      case 24:
        {
          uint8_t descriptor[TIGO_RADIO_DESCRIPTOR_LEN];
          if (loadBootstrapRadioDescriptor(descriptor) &&
              (!currentRadioDescriptorValid_ ||
               memcmp(currentRadioDescriptor_, descriptor, sizeof(descriptor)) != 0)) {
            addEvent("applying isolated lab bootstrap radio profile");
            startCcaCompatBootCommand(writeGatewayRadioDescriptor(descriptor));
            return;
          }
          ++bootCcaStep_;
          bootCcaRetryCount_ = 0;
          bootCcaNextActionMs_ = now + 100;
        }
        return;
      case 25:
        startCcaCompatBootCommand(requestNetworkStatus());
        return;
      case 26:
        startCcaCompatBootCommand(requestNodeTable(0));
        return;
      case 27:
        startCcaCompatBootCommand(requestGatewayLearnStart());
        return;
      case 28:
        startCcaCompatBootCommand(requestNodeTable(0));
        return;
      case 29:
        {
          uint8_t seed[TIGO_JOIN_SEED_LEN];
          if (loadBootstrapRadioJoinSeed(seed)) {
            storeJoinSeed(seed, sizeof(seed));
          }
        }
        if (!TIGO_SEND_41_DURING_STARTUP || !radioJoinSeedMatchesCurrentProfile()) {
          addEvent("cca startup join seed unavailable for current radio profile; step skipped");
          ++bootCcaStep_;
          bootCcaRetryCount_ = 0;
          bootCcaNextActionMs_ = now + 100;
          return;
        }
        startCcaCompatBootCommand(requestGatewayJoinSeed());
        return;
      case 30:
        nextPacketNumber_ = confirmedPacketCursor_;
        lastPollSentMs_ = platformMillis() - TIGO_RS485_POLL_INTERVAL_MS;
        bootCcaStep_ = UINT8_MAX;
        bootCcaRetryCount_ = 0;
        if (countPanelMapLongAddrs() > 0 && countValidNodeMap() == 0 &&
            lastNodeTableMs_ != 0 &&
            (lastNodeTableStart_ == 0 || lastNodeTableStart_ == TIGO_NODE_ID_BASE) &&
            lastNodeTableEntryCount_ == 0) {
          // The boot replay may have opened a learn window before the first
          // node table was seeded. Hand an empty, configured installation to
          // the recovery state machine so it writes 0x8002-style pending
          // entries before starting the authoritative learn window.
          beginNodeWakeSequence(now);
        } else if (lastNetworkStatusValid_ && lastNetworkMode_ == 1 && lastNetworkCountdown_ > 0) {
          nodeWakeActive_ = true;
          nodeWakeCompleted_ = false;
          nodeWakeLearnActive_ = true;
          nodeWakeLearnWaitCountdown_ = true;
          nodeWakeLearnStartedMs_ = now;
          nodeWakeLearnWarmupStep_ = ccaJoinBurstStepCount();
          nodeWakeLearnLastJoinSeedMs_ = radioJoinSeedMatchesCurrentProfile() ? now : 0;
          nodeWakeLearnLastNetworkStatusMs_ = now;
          nodeWakeNextActionMs_ = now + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
        }
        markPersistentStateDirty();
        addEvent("cca compat startup replay complete; polling starts at packet=%04X", nextPacketNumber_);
        return;
      default:
        bootCcaStep_ = UINT8_MAX;
        bootCcaWaitingStep_ = UINT8_MAX;
        return;
    }
  }

  void startCcaCompatBootCommand(bool started) {
    if (started) {
      bootCcaWaitingStep_ = bootCcaStep_;
    }
  }

  void handleCcaCompatBootCommandDone(TapCommandKind kind, bool ok) {
    if (!TIGO_CCA_COMPAT_BOOT_SEQUENCE || bootCcaWaitingStep_ == UINT8_MAX) {
      return;
    }
    if (kind != TapCommandKind::SimpleFrame &&
        kind != TapCommandKind::Enumerate &&
        kind != TapCommandKind::BootReceiveSeed &&
        kind != TapCommandKind::Version &&
        kind != TapCommandKind::RadioConfig &&
        kind != TapCommandKind::PvSubcommand &&
        kind != TapCommandKind::NetworkStatus &&
        kind != TapCommandKind::NodeTable) {
      return;
    }
    const uint8_t completedStep = bootCcaWaitingStep_;
    bootCcaWaitingStep_ = UINT8_MAX;
    if (completedStep == 0 && bootCcaWarmAttachPending_) {
      bootCcaWarmAttachPending_ = false;
      bootCcaRetryCount_ = 0;
      bootCcaNextActionMs_ = platformMillis() + 100;
      if (ok) {
        gatewayId_ = TIGO_DESIRED_GATEWAY_ID;
        bootCcaStep_ = 14;
        addEvent("cca warm attach succeeded; destructive TAP address dance skipped");
      } else {
        bootCcaStep_ = 0;
        addEvent("cca warm attach failed; full enumeration follows");
      }
      return;
    }
    if (ok && bootCcaStep_ == completedStep) {
      if (completedStep == 7 || completedStep == 13) {
        verifyAddressTransaction(gatewayId_);
      } else if (completedStep == 10) {
        verifyAddressTransaction(0x120A);
      } else if (completedStep == 6 || completedStep == 8 || completedStep == 9 || completedStep == 11) {
        acknowledgeAddressTransaction();
      }
      ++bootCcaStep_;
      bootCcaRetryCount_ = 0;
      bootCcaNextActionMs_ = platformMillis() + 100;
      addEvent("cca boot step %u ok", (unsigned)completedStep);
      return;
    }
    ++bootCcaRetryCount_;
    if (bootCcaRetryCount_ >= TIGO_CCA_BOOT_MAX_RETRIES_PER_STEP) {
      addEvent("cca boot step %u failed after %u retries; starting verified ID recovery",
               (unsigned)completedStep,
               (unsigned)bootCcaRetryCount_);
      failAddressTransaction();
      bootCcaStep_ = UINT8_MAX;
      bootCcaRetryCount_ = 0;
      beginReadOnlyWarmAttach(true);
      return;
    }
    bootCcaNextActionMs_ = platformMillis() + 1500;
    addEvent("cca boot step %u retry", (unsigned)completedStep);
  }

  bool traceReplayActive() const {
    return traceReplayState_ == TraceReplayState::Running ||
           traceReplayState_ == TraceReplayState::WaitingCommand ||
           traceReplayState_ == TraceReplayState::WaitingNoResponse ||
           traceReplayState_ == TraceReplayState::Holding;
  }

  uint16_t traceReplayTarget(const TraceReplayStep& step) const {
    return step.targetGatewayId == 0xFFFFU ? gatewayId_ : step.targetGatewayId;
  }

  void clearTraceReplayPlan() {
    memset(traceReplaySteps_, 0, sizeof(traceReplaySteps_));
    memset(traceReplayResults_, 0, sizeof(traceReplayResults_));
    traceReplayStepCount_ = 0;
    traceReplayStepIndex_ = 0;
    traceReplayState_ = TraceReplayState::Idle;
    traceReplayFailureReason_[0] = '\0';
    traceReplayReceivePumpEnabled_ = false;
    traceReplayUnexpectedResponse_ = false;
    traceReplayIdentityMismatch_ = false;
    traceReplayExpectedTapEui_[0] = '\0';
  }

  void leaveTraceReplayExclusiveMode(const char* reason) {
    replayExclusiveMode_ = false;
    recoveryAuthorized_ = false;
    readOnlyWarmAttachProtectsState_ = true;
    awaitingReceiveResponse_ = false;
    if (bootJournal_.rollbackNeeded) {
      addEvent("trace replay ended with unresolved address transaction; starting candidate recovery");
      beginReadOnlyWarmAttach(true);
    } else {
      lastPollSentMs_ = platformMillis() - TIGO_RS485_POLL_INTERVAL_MS;
    }
    addEvent("trace replay traffic released: %s", reason ? reason : "finished");
  }

  void failTraceReplay(const char* reason) {
    if (!traceReplayActive()) {
      return;
    }
    copyString(traceReplayFailureReason_, sizeof(traceReplayFailureReason_),
               reason ? reason : "trace replay failed");
    abortTapCommand(traceReplayFailureReason_);
    failAddressTransaction();
    traceReplayPollsAtEnd_ = pollsSent_;
    traceReplayState_ = TraceReplayState::Failed;
    addEvent("trace replay failed step=%u: %s",
             (unsigned)traceReplayStepIndex_, traceReplayFailureReason_);
    leaveTraceReplayExclusiveMode("failed");
  }

  void abortTraceReplay(const char* reason) {
    if (!traceReplayActive()) {
      return;
    }
    copyString(traceReplayFailureReason_, sizeof(traceReplayFailureReason_),
               reason ? reason : "trace replay aborted");
    abortTapCommand(traceReplayFailureReason_);
    traceReplayPollsAtEnd_ = pollsSent_;
    traceReplayState_ = TraceReplayState::Aborted;
    addEvent("trace replay aborted step=%u: %s",
             (unsigned)traceReplayStepIndex_, traceReplayFailureReason_);
    leaveTraceReplayExclusiveMode("aborted");
  }

  void completeTraceReplay() {
    if (bootJournal_.rollbackNeeded) {
      failTraceReplay("replay ended before address transaction verification");
      return;
    }
    traceReplayPollsAtEnd_ = pollsSent_;
    traceReplayState_ = TraceReplayState::Complete;
    addEvent("trace replay complete steps=%u duration=%lums polls=%lu",
             (unsigned)traceReplayStepCount_,
             (unsigned long)elapsed(platformMillis(), traceReplayStartedMs_),
             (unsigned long)(pollsSent_ - traceReplayPollsAtStart_));
    leaveTraceReplayExclusiveMode("complete");
  }

  void populateTraceReplayResult(uint8_t index,
                                 bool ok,
                                 TraceReplayOutcome actualOutcome,
                                 uint32_t now) {
    if (index >= TIGO_TRACE_REPLAY_MAX_STEPS) {
      return;
    }
    TraceReplayResult& result = traceReplayResults_[index];
    result.valid = true;
    result.ok = ok;
    result.actualOutcome = actualOutcome;
    result.completedOffsetMs = elapsed(now, traceReplayStartedMs_);
    result.responseType = lastCommandResponseType_;
    result.responseGatewayId = lastCommandResponseGatewayId_;
    result.responseDsn = lastCommandResponseDsn_;
    result.responseSubcmd = lastPvAckResponseSubcmd_;
    result.responseCredit = lastPvAckStatusFlags_;
  }

  void finishTraceReplayStep(TraceReplayOutcome actualOutcome, uint32_t now) {
    const uint8_t completed = traceReplayStepIndex_;
    populateTraceReplayResult(completed, true, actualOutcome, now);
    const TraceReplayStep& step = traceReplaySteps_[completed];
    addEvent("trace replay step=%u block=%u action=%s expected=%s actual=%s absolute_late=%ldms interstep_late=%ldms",
             (unsigned)completed,
             (unsigned)step.block,
             traceReplayActionName(step.action),
             traceReplayOutcomeName(step.outcome),
             traceReplayOutcomeName(actualOutcome),
             (long)traceReplayResults_[completed].startLateMs,
             (long)traceReplayResults_[completed].interStepLateMs);
    ++traceReplayStepIndex_;
    traceReplayState_ = TraceReplayState::Running;
  }

  bool traceReplayCommandMatches(const TraceReplayStep& step,
                                 TraceReplayOutcome* actualOutcome,
                                 char* reason,
                                 size_t reasonLen) {
    if (cursorMalformedPayloadCount_ != traceReplayBaselineCursorMalformed_) {
      copyString(reason, reasonLen, "malformed 0x0149 payload during replay");
      return false;
    }
    if (step.action != TraceReplayAction::ReceiveBootstrap &&
        (cursorDuplicateResponseCount_ != traceReplayBaselineCursorDuplicate_ ||
         cursorForwardResyncCount_ != traceReplayBaselineCursorForwardResync_)) {
      copyString(reason, reasonLen, "unexpected cursor resync during replay");
      return false;
    }
    const uint32_t currentRadioFingerprint = currentRadioDescriptorValid_
        ? fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) : 0;
    if (step.risk == TraceReplayRisk::ReadOnly &&
        traceReplayBaselineRadioFingerprint_ != 0 &&
        currentRadioFingerprint != traceReplayBaselineRadioFingerprint_) {
      copyString(reason, reasonLen, "read-only step changed radio fingerprint");
      return false;
    }
    const uint16_t expectedGateway = traceReplayExpectedGatewayId_;
    if (!lastCommandOk_) {
      if (step.outcome == TraceReplayOutcome::RfResponse &&
          lastCommandResponseType_ == 0x0146 &&
          lastCommandResponseGatewayId_ == expectedGateway) {
        *actualOutcome = TraceReplayOutcome::LocalTapAck;
        copyString(reason, reasonLen,
                   "local TAP 0x0146 ack received; expected RF response absent");
        return false;
      }
      copyString(reason, reasonLen, lastCommandMessage_);
      return false;
    }
    if (step.outcome == TraceReplayOutcome::LocalTapAck) {
      if (lastCommandResponseType_ != 0x0146 ||
          lastCommandResponseGatewayId_ != expectedGateway) {
        snprintf(reason, reasonLen, "expected local 0x0146 ack; got %04X/%04X",
                 lastCommandResponseGatewayId_, lastCommandResponseType_);
        return false;
      }
      *actualOutcome = TraceReplayOutcome::LocalTapAck;
      return true;
    }
    if (lastCommandResponseType_ != step.expectedType ||
        lastCommandResponseGatewayId_ != expectedGateway) {
      snprintf(reason, reasonLen, "response mismatch got=%04X/%04X expected=%04X/%04X",
               lastCommandResponseGatewayId_, lastCommandResponseType_,
               expectedGateway, step.expectedType);
      return false;
    }
    if (step.action != TraceReplayAction::PvSubcommand) {
      *actualOutcome = TraceReplayOutcome::FrameResponse;
      return true;
    }
    if (lastCommandResponseDsn_ != traceReplayExpectedDsn_) {
      snprintf(reason, reasonLen, "PV DSN mismatch got=%02X expected=%02X",
               lastCommandResponseDsn_, traceReplayExpectedDsn_);
      return false;
    }
    if (step.expectedPvSubcmd != 0 &&
        lastPvAckResponseSubcmd_ != step.expectedPvSubcmd) {
      snprintf(reason, reasonLen, "PV subcommand mismatch got=%02X expected=%02X",
               lastPvAckResponseSubcmd_, step.expectedPvSubcmd);
      return false;
    }
    const uint8_t requestSubcmd = (uint8_t)(step.typeCode & 0xFFU);
    const bool nodeResponse =
        requestSubcmd == 0x06 || requestSubcmd == 0x13 || requestSubcmd == 0x17;
    const bool nodeResponseHasBody = nodeResponse && lastPvAckBodyHex_[0] != '\0';
    if (step.outcome == TraceReplayOutcome::RfResponse) {
      if (!nodeResponseHasBody) {
        copyString(reason, reasonLen,
                   "empty TAP node response; genuine RF payload absent");
        return false;
      }
      if (step.expectedCredit != 0xFFU && lastPvAckStatusFlags_ != step.expectedCredit) {
        snprintf(reason, reasonLen, "RF credit mismatch got=%02X expected=%02X",
                 lastPvAckStatusFlags_, step.expectedCredit);
        return false;
      }
      *actualOutcome = TraceReplayOutcome::RfResponse;
      return true;
    }
    if (nodeResponseHasBody) {
      *actualOutcome = TraceReplayOutcome::RfResponse;
      copyString(reason, reasonLen,
                 "unexpected genuine RF node response for TAP-ack-only step");
      return false;
    }
    *actualOutcome = TraceReplayOutcome::TapAck;
    return true;
  }

  void captureTraceReplayTxTiming(TraceReplayResult& result,
                                  const TraceReplayStep& step,
                                  uint32_t dispatchEnteredMs,
                                  uint32_t scheduledTargetMs,
                                  uint32_t txGenerationBefore) {
    if (gatewayFrameTxGeneration_ == txGenerationBefore) {
      return;
    }
    result.dispatchPreparationMs = elapsed(lastGatewayFrameTxMs_, dispatchEnteredMs);
    result.actualStartOffsetMs = elapsed(lastGatewayFrameTxMs_, traceReplayStartedMs_);
    result.startLateMs = (int32_t)elapsed(
        lastGatewayFrameTxMs_, traceReplayStartedMs_ + step.offsetMs);
    result.interStepLateMs = (int32_t)elapsed(lastGatewayFrameTxMs_, scheduledTargetMs);
  }

  bool dispatchTraceReplayStep(const TraceReplayStep& step,
                               uint32_t now,
                               uint32_t scheduledTargetMs) {
    const uint16_t target = traceReplayTarget(step);
    const uint32_t txGenerationBefore = gatewayFrameTxGeneration_;
    TraceReplayResult& result = traceReplayResults_[traceReplayStepIndex_];
    memset(&result, 0, sizeof(result));
    result.valid = true;
    result.actualStartOffsetMs = elapsed(now, traceReplayStartedMs_);
    result.startLateMs = (int32_t)elapsed(
        now, traceReplayStartedMs_ + step.offsetMs);
    result.interStepLateMs = (int32_t)elapsed(now, scheduledTargetMs);
    traceReplayExpectedGatewayId_ = target;

    if (step.outcome == TraceReplayOutcome::NoResponse) {
      lastCommandResponseType_ = 0;
      lastCommandResponseGatewayId_ = 0;
      lastCommandResponseDsn_ = 0;
      lastPvAckResponseSubcmd_ = 0;
      lastPvAckStatusFlags_ = 0;
      if (step.risk == TraceReplayRisk::StateChanging &&
          step.typeCode != 0x0014) {
        addEvent("trace replay rejected unverified state-changing no-response frame type=%04X",
                 step.typeCode);
        return false;
      }
      if (step.typeCode == 0x0014 &&
          (bootJournal_.transactionState == (uint8_t)AddressTransactionState::None ||
           bootJournal_.transactionState == (uint8_t)AddressTransactionState::Verified)) {
        const uint16_t beforeId = bootJournal_.lastWorkingGatewayId != 0
            ? bootJournal_.lastWorkingGatewayId : gatewayId_;
        beginAddressTransaction(0x003C, beforeId, beforeId,
                                "trace enumeration burst pending assignment");
        if (!flushBootJournal(true)) {
          return false;
        }
      }
      if (step.action != TraceReplayAction::SimpleFrame ||
          !sendSimpleFrameNow(target, step.typeCode, step.payload, step.payloadLen)) {
        return false;
      }
      captureTraceReplayTxTiming(result, step, now, scheduledTargetMs,
                                 txGenerationBefore);
      traceReplayNoResponseType_ = step.expectedType;
      traceReplayNoResponseGatewayId_ = target;
      traceReplayUnexpectedResponse_ = false;
      const uint32_t defaultDeadline = now + 500UL;
      if (traceReplayStepIndex_ + 1U < traceReplayStepCount_) {
        const uint32_t nextDeadline = traceReplayStartedMs_ +
            traceReplaySteps_[traceReplayStepIndex_ + 1U].offsetMs;
        traceReplayNoResponseDeadlineMs_ = timeBefore(defaultDeadline, nextDeadline)
            ? defaultDeadline : nextDeadline;
      } else {
        traceReplayNoResponseDeadlineMs_ = defaultDeadline;
      }
      traceReplayState_ = TraceReplayState::WaitingNoResponse;
      return true;
    }

    traceReplayWaitingGeneration_ = lastCommandCompletionGeneration_ + 1U;
    bool started = false;
    switch (step.action) {
      case TraceReplayAction::SimpleFrame:
        if (step.typeCode == 0x003C && step.payloadLen >= 14) {
          const uint16_t desiredGatewayId =
              ((uint16_t)step.payload[12] << 8) | step.payload[13];
          started = sendGatewayAssignFrameCommand(target, desiredGatewayId,
                                                   "scheduled trace address assignment");
        } else if (step.typeCode == 0x0010 || step.typeCode == 0x0012) {
        started = sendMagicHandshakeFrame(step.typeCode,
                                          step.expectedType,
                                          "scheduled trace address handshake",
                                          target,
                                          false);
        } else {
          started = sendSimpleFrameCommandTo(target,
                                             step.typeCode,
                                             step.payload,
                                             step.payloadLen,
                                             step.expectedType,
                                             target,
                                             "scheduled trace frame",
                                             TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
        }
        break;
      case TraceReplayAction::ReceiveBootstrap:
        if (step.payloadLen == 5) {
          lastRequestedPacketNumber_ =
              ((uint16_t)step.payload[2] << 8) | step.payload[3];
          started = sendBootReceiveSeed(step.payload, step.payloadLen,
                                        "scheduled trace receive bootstrap");
        }
        break;
      case TraceReplayAction::PvSubcommand:
        started = sendPvSubcommand((uint8_t)(step.typeCode & 0xFFU),
                                   step.payload,
                                   step.payloadLen,
                                   step.expectedPvSubcmd);
        break;
    }
    if (!started) {
      return false;
    }
    captureTraceReplayTxTiming(result, step, now, scheduledTargetMs,
                               txGenerationBefore);
    traceReplayExpectedDsn_ = commandState_.expectedDsn;
    traceReplayCommandDeadlineMs_ = now + TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS + 250UL;
    traceReplayState_ = TraceReplayState::WaitingCommand;
    return true;
  }

  void serviceTraceReplayPollPump(uint32_t now, uint32_t nextActionMs) {
    if (!traceReplayReceivePumpEnabled_ || gatewayId_ == 0 ||
        tapCommandActive() || awaitingReceiveResponse_) {
      return;
    }
    if (timeBefore(now, nextActionMs) &&
        (uint32_t)(nextActionMs - now) <= traceReplayPollGuardMs_) {
      return;
    }
    if (elapsed(now, lastPollSentMs_) >= traceReplayPollIntervalMs_) {
      sendReceiveRequest();
    }
  }

  void processTraceReplay(uint32_t now) {
    if (!traceReplayActive()) {
      return;
    }
    if (traceReplayIdentityMismatch_) {
      failTraceReplay("TAP EUI changed during trace replay");
      return;
    }
    const bool bootstrapInFlight =
        traceReplayState_ == TraceReplayState::WaitingCommand &&
        traceReplayStepIndex_ < traceReplayStepCount_ &&
        traceReplaySteps_[traceReplayStepIndex_].action == TraceReplayAction::ReceiveBootstrap;
    if (cursorMalformedPayloadCount_ != traceReplayBaselineCursorMalformed_ ||
        (!bootstrapInFlight &&
         (cursorDuplicateResponseCount_ != traceReplayBaselineCursorDuplicate_ ||
          cursorForwardResyncCount_ != traceReplayBaselineCursorForwardResync_))) {
      failTraceReplay("cursor anomaly outside explicit receive bootstrap");
      return;
    }
    if (traceReplayUnexpectedResponse_) {
      char reason[96];
      snprintf(reason, sizeof(reason), "unexpected response %04X/%04X during no-response step",
               traceReplayNoResponseGatewayId_, traceReplayNoResponseType_);
      failTraceReplay(reason);
      return;
    }
    if (traceReplayState_ == TraceReplayState::WaitingNoResponse) {
      if (timeBefore(now, traceReplayNoResponseDeadlineMs_)) {
        return;
      }
      finishTraceReplayStep(TraceReplayOutcome::NoResponse, now);
    }
    if (traceReplayState_ == TraceReplayState::WaitingCommand) {
      if (tapCommandActive()) {
        if (timeReached(now, traceReplayCommandDeadlineMs_)) {
          failTraceReplay("command exceeded replay deadline");
        }
        return;
      }
      if (lastCommandCompletionGeneration_ < traceReplayWaitingGeneration_) {
        return;
      }
      TraceReplayOutcome actual = TraceReplayOutcome::FrameResponse;
      char reason[128];
      reason[0] = '\0';
      const TraceReplayStep& step = traceReplaySteps_[traceReplayStepIndex_];
      if (!traceReplayCommandMatches(step, &actual, reason, sizeof(reason))) {
        populateTraceReplayResult(traceReplayStepIndex_, false, actual, now);
        failTraceReplay(reason);
        return;
      }
      if (step.typeCode == 0x003C || step.typeCode == 0x0010 || step.typeCode == 0x0012) {
        acknowledgeAddressTransaction();
      } else if (step.action == TraceReplayAction::SimpleFrame && step.typeCode == 0x003A) {
        verifyAddressTransaction(traceReplayExpectedGatewayId_);
      }
      if (step.action == TraceReplayAction::ReceiveBootstrap) {
        traceReplayBaselineCursorDuplicate_ = cursorDuplicateResponseCount_;
        traceReplayBaselineCursorForwardResync_ = cursorForwardResyncCount_;
        traceReplayReceivePumpEnabled_ = true;
      }
      finishTraceReplayStep(actual, now);
    }
    if (traceReplayState_ != TraceReplayState::Running) {
      return;
    }
    if (traceReplayStepIndex_ >= traceReplayStepCount_) {
      if (elapsed(now, traceReplayStartedMs_) >= traceReplayHoldUntilOffsetMs_) {
        completeTraceReplay();
      } else {
        traceReplayState_ = TraceReplayState::Holding;
      }
      return;
    }
    const TraceReplayStep& step = traceReplaySteps_[traceReplayStepIndex_];
    const uint32_t absoluteTargetMs = traceReplayStartedMs_ + step.offsetMs;
    uint32_t scheduledTargetMs = absoluteTargetMs;
    if (traceReplayStepIndex_ > 0) {
      const uint8_t previousIndex = traceReplayStepIndex_ - 1U;
      const TraceReplayResult& previousResult = traceReplayResults_[previousIndex];
      const TraceReplayStep& previousStep = traceReplaySteps_[previousIndex];
      if (previousResult.valid) {
        const uint32_t sourceDeltaMs = step.offsetMs - previousStep.offsetMs;
        const uint32_t responseAwareTargetMs = traceReplayStartedMs_ +
            previousResult.completedOffsetMs + sourceDeltaMs;
        if (timeBefore(scheduledTargetMs, responseAwareTargetMs)) {
          scheduledTargetMs = responseAwareTargetMs;
        }
      }
    }
    if (timeBefore(now, scheduledTargetMs)) {
      serviceTraceReplayPollPump(now, scheduledTargetMs);
      return;
    }
    if (awaitingReceiveResponse_) {
      return;
    }
    const uint32_t lateMs = elapsed(now, scheduledTargetMs);
    if (lateMs > traceReplayMaxLateMs_) {
      char reason[96];
      snprintf(reason, sizeof(reason), "step %u interstep late by %lums (limit %lums)",
               (unsigned)traceReplayStepIndex_,
               (unsigned long)lateMs,
               (unsigned long)traceReplayMaxLateMs_);
      failTraceReplay(reason);
      return;
    }
    if (!dispatchTraceReplayStep(step, now, scheduledTargetMs)) {
      failTraceReplay("step dispatch rejected or transport busy");
    }
  }

  void processTraceReplayHold(uint32_t now) {
    if (traceReplayState_ != TraceReplayState::Holding) {
      return;
    }
    const uint32_t holdEndMs = traceReplayStartedMs_ + traceReplayHoldUntilOffsetMs_;
    if (timeReached(now, holdEndMs)) {
      completeTraceReplay();
      return;
    }
    serviceTraceReplayPollPump(now, holdEndMs);
  }

  void observeTraceReplayFrame(const GatewayFrame& frame) {
    if (traceReplayActive() && frame.fromGateway && frame.crcOk &&
        (frame.typeCode == 0x0039 || frame.typeCode == 0x003B) &&
        frame.payloadLen >= 8 && traceReplayExpectedTapEui_[0] != '\0') {
      char observedEui[17];
      bytesToHex(frame.payload, 8, observedEui, sizeof(observedEui));
      if (strcmp(observedEui, traceReplayExpectedTapEui_) != 0) {
        traceReplayIdentityMismatch_ = true;
      }
    }
    if (traceReplayState_ != TraceReplayState::WaitingNoResponse ||
        !frame.valid || !frame.crcOk || !frame.fromGateway) {
      return;
    }
    if (frame.gatewayId == traceReplayNoResponseGatewayId_ &&
        frame.typeCode == traceReplayNoResponseType_) {
      traceReplayUnexpectedResponse_ = true;
    }
  }

  bool nodeIdByWakeIndex(uint16_t wakeIndex, uint16_t* nodeId) const {
    if (nodeId == nullptr) {
      return false;
    }
    uint16_t seen = 0;
    const uint16_t maxNodes = (TIGO_CCA_NODE_WAKE_MAX_NODES < TIGO_MAX_OPTIMIZERS)
        ? TIGO_CCA_NODE_WAKE_MAX_NODES
        : TIGO_MAX_OPTIMIZERS;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid || nodeMap_[i].pending || nodeMap_[i].nodeId == 0) {
        continue;
      }
      if (seen >= maxNodes) {
        return false;
      }
      if (seen == wakeIndex) {
        *nodeId = nodeMap_[i].nodeId;
        return true;
      }
      ++seen;
    }
    return false;
  }

  uint16_t countWakeCandidateNodes() const {
    return countConfirmedNodeMap();
  }

  bool isPreferredCcaNode(uint16_t nodeId) const {
    static const uint16_t preferred[] = {11, 7, 9, 10, 3, 6, 2, 8, 4, 5};
    for (uint16_t preferredNode : preferred) {
      if (nodeId == preferredNode) {
        return true;
      }
    }
    return false;
  }

  bool nodeIdByCcaWakeIndex(uint16_t wakeIndex, uint16_t* nodeId) const {
    if (nodeId == nullptr) {
      return false;
    }
    uint16_t seen = 0;
    static const uint16_t preferred[] = {11, 7, 9, 10, 3, 6, 2, 8, 4, 5};
    for (uint16_t preferredNode : preferred) {
      if (lookupLongAddrForNode(preferredNode) == nullptr) {
        continue;
      }
      if (seen == wakeIndex) {
        *nodeId = preferredNode;
        return true;
      }
      ++seen;
    }
    const uint16_t maxNodes = (TIGO_CCA_NODE_WAKE_MAX_NODES < TIGO_MAX_OPTIMIZERS)
        ? TIGO_CCA_NODE_WAKE_MAX_NODES
        : TIGO_MAX_OPTIMIZERS;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid || nodeMap_[i].pending || nodeMap_[i].nodeId == 0 || isPreferredCcaNode(nodeMap_[i].nodeId)) {
        continue;
      }
      if (seen >= maxNodes) {
        return false;
      }
      if (seen == wakeIndex) {
        *nodeId = nodeMap_[i].nodeId;
        return true;
      }
      ++seen;
    }
    return false;
  }

  uint16_t ccaPhaseSlotsForNode(uint16_t nodeId, uint16_t fallbackIndex) const {
    switch (nodeId) {
      case 2: return 0x0000;
      case 3: return 0x0028;
      case 4: return 0x0050;
      case 5: return 0x0078;
      case 6: return 0x00A0;
      case 7: return 0x00C8;
      case 8: return 0x00F0;
      case 9: return 0x0118;
      case 10: return 0x0140;
      case 11: return 0x0168;
      default:
        return (uint16_t)(fallbackIndex * TIGO_CCA_NODE_WAKE_PV_PHASE_STEP_SLOTS);
    }
  }

  static constexpr uint16_t ccaJoinBurstStepCount() {
    return TIGO_CCA_JOIN_BURST_DURING_WAKE
        ? (uint16_t)(2U + (TIGO_CCA_JOIN_BURST_REPEATS * 2U))
        : 0U;
  }

  bool requestCcaJoinBurstStep(uint16_t step) {
    if (step == 0) {
      return requestGatewayTableClear();
    }
    if (step == 1) {
      return requestGatewayLearnStart();
    }
    if (((step - 2U) & 0x01U) == 0) {
      return requestGatewayJoinSeed();
    }
    return requestNetworkStatus();
  }

  void beginNodeSeedRecovery(uint32_t now) {
    forceLearnAfterProfileWrite_ = false;
    // Clearing resets the TAP allocation base to node 2. Packet 0x22 controls
    // PV run/off state and must not be used as a table selector here.
    nodeSeedState_ = NodeSeedState::ClearTable;
    nodeSeedAwaitingCommand_ = false;
    nodeSeedNextPanelIndex_ = 0;
    nodeSeedPanelCount_ = countPanelMapLongAddrs();
    const uint16_t chunkSize = TIGO_NODE_SEED_CHUNK_SIZE == 0 ? 1 : TIGO_NODE_SEED_CHUNK_SIZE;
    nodeSeedChunksTotal_ = (uint16_t)((nodeSeedPanelCount_ + chunkSize - 1U) / chunkSize);
    nodeSeedChunksAcked_ = 0;
    nodeSeedRetryCount_ = 0;
    nodeSeedVerifyStart_ = 0;
    nodeSeedVerifiedReadbackEntries_ = 0;
    lastSeedError_[0] = '\0';
    nodeWakeNextActionMs_ = now;
    addEvent("node seed recovery start; panels=%u chunks=%u chunk_size=%u",
             nodeSeedPanelCount_,
             nodeSeedChunksTotal_,
             (unsigned)TIGO_NODE_SEED_CHUNK_SIZE);
  }

  void beginLearnForExistingTable(uint32_t now, const char* reason) {
    if (radioProfileMismatch()) {
      nodeWakeActive_ = false;
      nodeWakeCompleted_ = false;
      nodeSeedState_ = NodeSeedState::Failed;
      copyString(lastSeedError_, sizeof(lastSeedError_),
                 "TAP radio profile differs from proven working profile");
      lastNodeWakeAttemptMs_ = now;
      addEvent("learn blocked: current TAP radio profile differs from working profile");
      return;
    }
    nodeWakeActive_ = true;
    nodeWakeCompleted_ = false;
    nodeWakeLearnActive_ = false;
    nodeWakeLearnWaitCountdown_ = false;
    nodeWakeSkipLearn_ = false;
    nodeWakeLearnRestartAfterPvRun_ = false;
    forceLearnUntilMs_ = 0;
    nodeSeedState_ = NodeSeedState::StartLearn;
    nodeSeedAwaitingCommand_ = false;
    nodeSeedPanelCount_ = lastNetworkExpectedNodes_ > 0
        ? lastNetworkExpectedNodes_ : countPanelMapLongAddrs();
    if (nodeSeedPanelCount_ == 0) {
      nodeSeedPanelCount_ = lastNodeTableEntryCount_;
    }
    nodeWakeNextActionMs_ = now;
    nodeWakeLearnStartedMs_ = 0;
    nodeWakeLearnLastNetworkStatusMs_ = 0;
    nodeWakeLearnLastJoinSeedMs_ = 0;
    nodeWakeLearnWarmupStep_ = 0;
    lastNodeWakeAttemptMs_ = now;
    addEvent("learn restart for existing node table: %s nodes=%u",
             reason ? reason : "requested", nodeSeedPanelCount_);
  }

  void beginForcedLearnWindow(uint32_t now) {
    if (radioProfileMismatch()) {
      addEvent("forced learn blocked: current TAP radio profile differs from working profile");
      return;
    }
    nodeWakeActive_ = true;
    nodeWakeCompleted_ = false;
    nodeWakeLearnActive_ = false;
    nodeWakeLearnWaitCountdown_ = false;
    nodeWakeSkipLearn_ = false;
    nodeWakeLearnRestartAfterPvRun_ = false;
    nodeSeedState_ = NodeSeedState::StartLearn;
    nodeSeedAwaitingCommand_ = false;
    nodeSeedPanelCount_ = lastNetworkExpectedNodes_ > 0
        ? lastNetworkExpectedNodes_ : countPanelMapLongAddrs();
    if (nodeSeedPanelCount_ == 0) {
      nodeSeedPanelCount_ = lastNodeTableEntryCount_;
    }
    nodeWakeNextActionMs_ = now;
    nodeWakeLearnStartedMs_ = 0;
    nodeWakeLearnLastJoinSeedMs_ = 0;
    nodeWakeLearnLastNetworkStatusMs_ = 0;
    nodeWakeLearnWarmupStep_ = 0;
    forceLearnUntilMs_ = now + TIGO_CCA_LEARN_COUNTDOWN_TIMEOUT_MS;
    lastNodeWakeAttemptMs_ = now;
    if (!requestGatewayLearnStart()) {
      forceLearnUntilMs_ = 0;
      nodeSeedState_ = NodeSeedState::Failed;
      addEvent("forced learn could not queue TAP learn command");
      return;
    }
    nodeSeedAwaitingCommand_ = true;
    addEvent("forced learn window queued for %lu ms; PV run enable follows learn ack",
             (unsigned long)TIGO_CCA_LEARN_COUNTDOWN_TIMEOUT_MS);
  }

  void failNodeSeedRecovery(const char* reason) {
    nodeSeedState_ = NodeSeedState::Failed;
    nodeSeedAwaitingCommand_ = false;
    copyString(lastSeedError_, sizeof(lastSeedError_), reason ? reason : "failed");
    addEvent("node seed recovery failed: %s", lastSeedError_);
  }

  void handleNodeSeedCommandDone(TapCommandKind kind, uint16_t arg0, bool ok) {
    if (!nodeSeedAwaitingCommand_) {
      return;
    }
    nodeSeedAwaitingCommand_ = false;
    if (!ok) {
      if (nodeSeedRetryCount_ < 3) {
        ++nodeSeedRetryCount_;
        nodeWakeNextActionMs_ = platformMillis() + 1500;
        addEvent("node seed state=%s retry=%u", nodeSeedStateName(), (unsigned)nodeSeedRetryCount_);
        return;
      }
      failNodeSeedRecovery(lastCommandMessage_);
      return;
    }
    nodeSeedRetryCount_ = 0;
    if (nodeSeedState_ == NodeSeedState::ClearTable && kind == TapCommandKind::PvSubcommand && arg0 == 0x2B) {
      nodeSeedState_ = NodeSeedState::SendChunk;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      addEvent("node table clear ack 0x2C received; allocation base reset to node 2");
      return;
    }
    if (nodeSeedState_ == NodeSeedState::SendChunk && kind == TapCommandKind::PvSubcommand && arg0 == 0x29) {
      const uint16_t chunkSize = TIGO_NODE_SEED_CHUNK_SIZE == 0 ? 1 : TIGO_NODE_SEED_CHUNK_SIZE;
      nodeSeedNextPanelIndex_ = (uint16_t)(nodeSeedNextPanelIndex_ + chunkSize);
      ++nodeSeedChunksAcked_;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      addEvent("node seed chunk ack 0x2A received; acked=%u/%u",
               nodeSeedChunksAcked_,
               nodeSeedChunksTotal_);
      if (nodeSeedNextPanelIndex_ >= nodeSeedPanelCount_) {
        nodeSeedState_ = TIGO_VERIFY_NODE_TABLE_AFTER_SEED ? NodeSeedState::VerifyNodeTable : NodeSeedState::Done;
        nodeSeedVerifyStart_ = 0;
        nodeSeedVerifiedReadbackEntries_ = 0;
      }
      return;
    }
    if (nodeSeedState_ == NodeSeedState::VerifyNodeTable && kind == TapCommandKind::NodeTable) {
      if (lastNodeTableEntryCount_ > 0) {
        nodeSeedVerifiedReadbackEntries_ = (uint16_t)(nodeSeedVerifiedReadbackEntries_ + lastNodeTableEntryCount_);
      }
      addEvent("node table verify page start=%u count=%u readback=%u expected=%u node_count=%u",
               lastNodeTableStart_,
               lastNodeTableEntryCount_,
               nodeSeedVerifiedReadbackEntries_,
               nodeSeedPanelCount_,
               countValidNodeMap());
      if (nodeSeedVerifiedReadbackEntries_ >= nodeSeedPanelCount_) {
        if (!nodeTableMatchesConfiguredPanels(true)) {
          failNodeSeedRecovery("node table differs from pending seed entries");
          return;
        }
        nodeSeedState_ = TIGO_SEND_2D_DURING_STARTUP ? NodeSeedState::StartLearn : NodeSeedState::Done;
        addEvent("pending node seed verified exactly; readback=%u expected=%u",
                 nodeSeedVerifiedReadbackEntries_, nodeSeedPanelCount_);
        nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
        return;
      }
      if (lastNodeTableEntryCount_ == 0) {
        failNodeSeedRecovery(nodeSeedVerifiedReadbackEntries_ == 0 ? "node table empty after seed" : "node table ended before expected nodes");
        return;
      }
      nodeSeedVerifyStart_ = (uint16_t)(lastNodeTableStart_ + lastNodeTableEntryCount_);
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      return;
    }
    if (nodeSeedState_ == NodeSeedState::StartLearn &&
        kind == TapCommandKind::PvSubcommand && arg0 == 0x2D) {
      nodeSeedState_ = NodeSeedState::EnablePv;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      addEvent("optimizer learn window accepted; enabling PV run state");
      return;
    }
    if (nodeSeedState_ == NodeSeedState::EnablePv &&
        kind == TapCommandKind::PvSubcommand && arg0 == 0x22) {
      nodeSeedState_ = NodeSeedState::Done;
      nodeWakeLearnActive_ = true;
      nodeWakeLearnWaitCountdown_ = TIGO_WAIT_FOR_LEARN_COUNTDOWN_AFTER_SEED;
      nodeWakeLearnStartedMs_ = platformMillis();
      nodeWakeLearnWarmupStep_ = ccaJoinBurstStepCount();
      nodeWakeLearnLastJoinSeedMs_ = 0;
      nodeWakeLearnLastNetworkStatusMs_ = 0;
      nodeWakeLearnRestartAfterPvRun_ = false;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      addEvent("optimizer learn window active with PV run enabled; waiting for pending nodes to confirm");
      return;
    }
  }

  bool processNodeSeedRecovery(uint32_t now) {
    if (!TIGO_ENABLE_NODE_SEED ||
        nodeSeedState_ == NodeSeedState::Idle ||
        nodeSeedState_ == NodeSeedState::Done ||
        nodeSeedState_ == NodeSeedState::Failed) {
      return false;
    }
    if (nodeSeedAwaitingCommand_) {
      return true;
    }
    if (timeBefore(now, nodeWakeNextActionMs_)) {
      return true;
    }
    if (nodeSeedPanelCount_ == 0) {
      failNodeSeedRecovery("panel map empty");
      return true;
    }
    switch (nodeSeedState_) {
      case NodeSeedState::ClearTable:
        if (requestGatewayTableClear()) {
          nodeSeedAwaitingCommand_ = true;
        }
        return true;
      case NodeSeedState::SendChunk:
        if (nodeSeedNextPanelIndex_ >= nodeSeedPanelCount_) {
          nodeSeedState_ = TIGO_VERIFY_NODE_TABLE_AFTER_SEED ? NodeSeedState::VerifyNodeTable : NodeSeedState::Done;
          nodeSeedVerifyStart_ = 0;
          return true;
        }
        if (requestGatewayNodeSeedFromPanelMapChunk(nodeSeedNextPanelIndex_)) {
          nodeSeedAwaitingCommand_ = true;
        }
        return true;
      case NodeSeedState::VerifyNodeTable:
        if (requestNodeTable(nodeSeedVerifyStart_)) {
          nodeSeedAwaitingCommand_ = true;
        }
        return true;
      case NodeSeedState::StartLearn:
        if (nodeSeedPanelCount_ == 0 && lastNetworkExpectedNodes_ == 0 &&
            countPanelMapLongAddrs() == 0 && countValidNodeMap() == 0) {
          failNodeSeedRecovery("learn expected node count unavailable");
          return true;
        }
        if (requestGatewayLearnStart()) {
          nodeSeedAwaitingCommand_ = true;
        }
        return true;
      case NodeSeedState::EnablePv:
        if (requestPvRunState(true)) {
          nodeSeedAwaitingCommand_ = true;
        }
        return true;
      default:
        return false;
    }
  }

  void beginNodeWakeSequence(uint32_t now, bool forcePvConfig = false) {
    if (radioProfileMismatch()) {
      nodeWakeActive_ = false;
      nodeWakeCompleted_ = false;
      nodeSeedState_ = NodeSeedState::Failed;
      copyString(lastSeedError_, sizeof(lastSeedError_),
                 "TAP radio profile differs from proven working profile");
      lastNodeWakeAttemptMs_ = now;
      addEvent("startup blocked: current TAP radio profile differs from working profile; explicit apply required");
      return;
    }
    nodeWakeActive_ = true;
    nodeWakeCompleted_ = false;
    nodeWakeLearnActive_ = false;
    nodeWakeLearnWaitCountdown_ = false;
    nodeWakeSkipLearn_ = false;
    nodeWakeConfigFallbackActive_ = false;
    nodeWakeForcePvConfig_ = forcePvConfig;
    sequence_ = 0;
    nodeWakeStep_ = 0;
    nodeWakeNextActionMs_ = now;
    nodeWakeLearnStartedMs_ = nodeWakeLearnActive_ ? now : 0;
    nodeWakeLearnLastNetworkStatusMs_ = 0;
    nodeWakeLearnLastJoinSeedMs_ = 0;
    nodeWakeLearnWarmupStep_ = 0;
    lastNodeWakeAttemptMs_ = now;
    const bool shouldSeedNodeMap =
        TIGO_ENABLE_NODE_SEED &&
        countPanelMapLongAddrs() > 0 &&
        lastNodeTableMs_ != 0 &&
        (lastNodeTableStart_ == 0 || lastNodeTableStart_ == TIGO_NODE_ID_BASE) &&
        lastNodeTableEntryCount_ == 0 &&
        countValidNodeMap() == 0;
    if (countPendingNodeMap() > 0 && countConfirmedNodeMap() == 0) {
      nodeSeedState_ = NodeSeedState::StartLearn;
      nodeSeedAwaitingCommand_ = false;
      nodeSeedPanelCount_ = countPendingNodeMap();
      addEvent("pending node table found; preserving entries and restarting learn window");
    } else if (shouldSeedNodeMap) {
      beginNodeSeedRecovery(now);
    } else {
      nodeSeedState_ = NodeSeedState::Idle;
      nodeSeedAwaitingCommand_ = false;
    }
    addEvent("cca node wake sequence start; nodes=%u source=%s force_pv=%u learn=%u",
             countWakeCandidateNodes(),
             countConfirmedNodeMap() > 0 ? "confirmed" :
             (countPendingNodeMap() > 0 ? "pending" : "empty"),
             forcePvConfig ? 1U : 0U,
             nodeWakeLearnActive_ ? 1U : 0U);
  }

  void finishNodeWakeSequence(bool complete, const char* reason) {
    nodeWakeActive_ = false;
    nodeWakeCompleted_ = complete;
    nodeWakeLearnActive_ = false;
    nodeWakeLearnWaitCountdown_ = false;
    nodeWakeSkipLearn_ = false;
    nodeWakeLearnRestartAfterPvRun_ = false;
    forceLearnUntilMs_ = 0;
    nodeSeedAwaitingCommand_ = false;
    nodeWakeForcePvConfig_ = false;
    lastNodeWakeAttemptMs_ = platformMillis();
    addEvent("cca node wake sequence %s: %s", complete ? "complete" : "stopped", reason ? reason : "");
  }

  NodeMapEntry* findRfNodePromotionCandidate(uint32_t now) {
    if (!lastNetworkStatusValid_ ||
        lastNetworkConfirmedNodes_ >= countConfirmedNodeMap()) {
      return nullptr;
    }
    static constexpr uint32_t MAX_RF_PROOF_AGE_MS = 120000UL;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid || nodeMap_[i].pending ||
          !nodeMap_[i].rfConfirmed || nodeMap_[i].nodeId == 0 ||
          nodeMap_[i].rawNodeId != nodeMap_[i].nodeId ||
          elapsed(now, nodeMap_[i].updatedMs) > MAX_RF_PROOF_AGE_MS) {
        continue;
      }
      return &nodeMap_[i];
    }
    return nullptr;
  }

  bool rfNodePromotionTargetCommitted() const {
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid &&
          nodeMap_[i].nodeId == rfNodePromotionNodeId_ &&
          nodeMap_[i].rawNodeId == rfNodePromotionNodeId_ &&
          !nodeMap_[i].pending &&
          strcmp(nodeMap_[i].longAddr, rfNodePromotionLongAddr_) == 0) {
        return true;
      }
    }
    return false;
  }

  void resetRfNodePromotionProof() {
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid &&
          nodeMap_[i].nodeId == rfNodePromotionNodeId_) {
        nodeMap_[i].rfConfirmed = false;
      }
    }
    rfNodePromotionState_ = RfNodePromotionState::Idle;
    rfNodePromotionNodeId_ = 0;
    rfNodePromotionLongAddr_[0] = '\0';
  }

  bool beginRfNodePromotion(NodeMapEntry* candidate) {
    if (candidate == nullptr || rfNodePromotionState_ != RfNodePromotionState::Idle) {
      return false;
    }
    rfNodePromotionNodeId_ = candidate->nodeId;
    copyString(rfNodePromotionLongAddr_, sizeof(rfNodePromotionLongAddr_),
               candidate->longAddr);
    if (!requestGatewayLearnCancel()) {
      rfNodePromotionNodeId_ = 0;
      rfNodePromotionLongAddr_[0] = '\0';
      return false;
    }
    rfNodePromotionState_ = RfNodePromotionState::CancelWaiting;
    addEvent("RF-confirmed node %u promotion started; learn cancel queued",
             rfNodePromotionNodeId_);
    return true;
  }

  bool processRfNodePromotion() {
    switch (rfNodePromotionState_) {
      case RfNodePromotionState::Idle:
        return false;
      case RfNodePromotionState::WriteReady:
        if (requestGatewayConfirmedNodeWrite(rfNodePromotionNodeId_,
                                             rfNodePromotionLongAddr_)) {
          rfNodePromotionState_ = RfNodePromotionState::WriteWaiting;
        }
        return true;
      case RfNodePromotionState::VerifyReady:
        if (requestNodeTable(0)) {
          rfNodePromotionState_ = RfNodePromotionState::VerifyWaiting;
        }
        return true;
      case RfNodePromotionState::StatusReady:
        if (requestNetworkStatus()) {
          rfNodePromotionState_ = RfNodePromotionState::StatusWaiting;
        }
        return true;
      default:
        return true;
    }
  }

  void handleRfNodePromotionCommandDone(TapCommandKind kind,
                                        uint16_t arg0,
                                        bool ok) {
    if (rfNodePromotionState_ == RfNodePromotionState::Idle) {
      return;
    }
    if (!ok) {
      addEvent("RF-confirmed node %u promotion failed", rfNodePromotionNodeId_);
      resetRfNodePromotionProof();
      return;
    }
    if (rfNodePromotionState_ == RfNodePromotionState::CancelWaiting &&
        kind == TapCommandKind::PvSubcommand && arg0 == 0x2D) {
      rfNodePromotionState_ = RfNodePromotionState::WriteReady;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      return;
    }
    if (rfNodePromotionState_ == RfNodePromotionState::WriteWaiting &&
        kind == TapCommandKind::PvSubcommand && arg0 == 0x29) {
      rfNodePromotionState_ = RfNodePromotionState::VerifyReady;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      return;
    }
    if (rfNodePromotionState_ == RfNodePromotionState::VerifyWaiting &&
        kind == TapCommandKind::NodeTable) {
      if (!rfNodePromotionTargetCommitted()) {
        addEvent("RF-confirmed node %u promotion readback stayed pending",
                 rfNodePromotionNodeId_);
        resetRfNodePromotionProof();
        return;
      }
      rfNodePromotionState_ = RfNodePromotionState::StatusReady;
      nodeWakeNextActionMs_ = platformMillis() + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
      return;
    }
    if (rfNodePromotionState_ == RfNodePromotionState::StatusWaiting &&
        kind == TapCommandKind::NetworkStatus) {
      if (lastNetworkExpectedNodes_ == 0 ||
          lastNetworkConfirmedNodes_ < lastNetworkExpectedNodes_) {
        addEvent("RF-confirmed node %u committed but network remains %u/%u",
                 rfNodePromotionNodeId_,
                 lastNetworkConfirmedNodes_,
                 lastNetworkExpectedNodes_);
        resetRfNodePromotionProof();
        return;
      }
      const uint16_t promotedNodeId = rfNodePromotionNodeId_;
      rfNodePromotionState_ = RfNodePromotionState::Idle;
      rfNodePromotionNodeId_ = 0;
      rfNodePromotionLongAddr_[0] = '\0';
      nodeWakeLearnActive_ = false;
      nodeWakeLearnWaitCountdown_ = false;
      nodeSeedState_ = NodeSeedState::Done;
      nodeWakeStep_ = 0;
      nodeWakeNextActionMs_ = platformMillis();
      addEvent("RF-confirmed node %u committed; network=%u/%u; reporting setup continues",
               promotedNodeId,
               lastNetworkConfirmedNodes_,
               lastNetworkExpectedNodes_);
    }
  }

  uint8_t ccaWakeActionFor(uint16_t round, uint16_t nodeIndex) const {
    static const uint8_t actionTable[4][4] = {
      {0, 3, 1, 2},
      {1, 2, 0, 3},
      {3, 1, 2, 0},
      {2, 0, 3, 1},
    };
    return actionTable[round & 0x03U][nodeIndex & 0x03U];
  }

  bool processNodeWakeLearnPhase(uint32_t now) {
    if (!nodeWakeLearnActive_) {
      return false;
    }
    const bool forcedLearnActive = forceLearnUntilMs_ != 0 && timeBefore(now, forceLearnUntilMs_);
    if (forceLearnUntilMs_ != 0 && !forcedLearnActive) {
      forceLearnUntilMs_ = 0;
      const bool tapLearnStillActive =
          nodeWakeLearnWaitCountdown_ &&
          lastNetworkStatusValid_ &&
          lastNetworkMode_ != 0 &&
          lastNetworkCountdown_ > 0;
      if (tapLearnStillActive) {
        addEvent("forced learn guard elapsed; continuing TAP learn countdown=%u",
                 lastNetworkCountdown_);
      } else {
        addEvent("forced learn window elapsed");
        finishNodeWakeSequence(false, "forced learn window elapsed");
        return true;
      }
    }
    if (processRfNodePromotion()) {
      return true;
    }
    NodeMapEntry* promotionCandidate = TIGO_PROMOTE_RF_CONFIRMED_NODES
        ? findRfNodePromotionCandidate(now)
        : nullptr;
    if (promotionCandidate != nullptr &&
        beginRfNodePromotion(promotionCandidate)) {
      return true;
    }
    if (nodeWakeLearnWaitCountdown_) {
      const bool networkStatusAuthoritative =
          lastNetworkStatusValid_ && lastNetworkExpectedNodes_ > 0;
      const uint16_t confirmed = networkStatusAuthoritative
          ? lastNetworkConfirmedNodes_ : countConfirmedNodeMap();
      const uint16_t expected = lastNetworkExpectedNodes_ > 0
          ? lastNetworkExpectedNodes_
          : (nodeSeedPanelCount_ > 0 ? nodeSeedPanelCount_ : countPanelMapLongAddrs());
      if (!forcedLearnActive && expected > 0 && confirmed >= expected) {
        nodeWakeLearnActive_ = false;
        nodeWakeLearnWaitCountdown_ = false;
        nodeWakeStep_ = ccaJoinBurstStepCount();
        nodeWakeNextActionMs_ = now;
        addEvent("optimizer learn complete; confirmed=%u expected=%u", confirmed, expected);
        return false;
      }
      if (lastNetworkStatusValid_ &&
          (lastNetworkMode_ == 0 || lastNetworkCountdown_ == 0) &&
          confirmed < expected) {
        if (!nodeWakeLearnRestartAfterPvRun_) {
          if (requestPvRunState(true)) {
            nodeWakeLearnRestartAfterPvRun_ = true;
            addEvent("learn window expired with %u/%u confirmed; PV run reasserted before restart",
                     confirmed, expected);
          }
          return true;
        }
        if (requestGatewayLearnStart()) {
          nodeWakeLearnRestartAfterPvRun_ = false;
          nodeWakeLearnStartedMs_ = now;
          nodeWakeLearnLastNetworkStatusMs_ = now;
          addEvent("learn window expired with %u/%u confirmed; restarted", confirmed, expected);
        }
        return true;
      }
      if (elapsed(now, nodeWakeLearnStartedMs_) >= TIGO_CCA_LEARN_COUNTDOWN_TIMEOUT_MS) {
        if (!nodeWakeLearnRestartAfterPvRun_) {
          if (requestPvRunState(true)) {
            nodeWakeLearnRestartAfterPvRun_ = true;
            addEvent("learn watchdog PV run refresh before restart; confirmed=%u expected=%u",
                     confirmed, expected);
          }
          return true;
        }
        if (requestGatewayLearnStart()) {
          nodeWakeLearnRestartAfterPvRun_ = false;
          nodeWakeLearnStartedMs_ = now;
          addEvent("learn window watchdog restart; confirmed=%u expected=%u", confirmed, expected);
        }
        return true;
      }
      if (TIGO_SEND_41_DURING_STARTUP && radioJoinSeedMatchesCurrentProfile() &&
          (nodeWakeLearnLastJoinSeedMs_ == 0 ||
           elapsed(now, nodeWakeLearnLastJoinSeedMs_) >= TIGO_CCA_LEARN_JOIN_SEED_EVERY_MS)) {
        if (requestGatewayJoinSeed()) {
          nodeWakeLearnLastJoinSeedMs_ = now;
        }
        return true;
      }
      if (nodeWakeLearnLastNetworkStatusMs_ == 0 ||
          elapsed(now, nodeWakeLearnLastNetworkStatusMs_) >= TIGO_CCA_LEARN_NETWORK_STATUS_EVERY_MS) {
        if (requestNetworkStatus()) {
          nodeWakeLearnLastNetworkStatusMs_ = now;
        }
        return true;
      }
      return true;
    }
    const bool networkStatusAuthoritative =
        lastNetworkStatusValid_ && lastNetworkExpectedNodes_ > 0;
    const uint16_t confirmed = networkStatusAuthoritative
        ? lastNetworkConfirmedNodes_ : countConfirmedNodeMap();
    const uint16_t expected = lastNetworkExpectedNodes_ > 0
        ? lastNetworkExpectedNodes_
        : (nodeSeedPanelCount_ > 0 ? nodeSeedPanelCount_ : countPanelMapLongAddrs());
    if ((expected > 0 && confirmed >= expected) || (expected == 0 && confirmed > 0)) {
      nodeWakeLearnActive_ = false;
      nodeWakeStep_ = 0;
      nodeWakeNextActionMs_ = now;
      addEvent("cca learn complete; configured=%u active=%u node_map=%u",
               expected,
               confirmed,
               countValidNodeMap());
      return false;
    }
    if (elapsed(now, nodeWakeLearnStartedMs_) >= TIGO_CCA_LEARN_TIMEOUT_MS) {
      nodeWakeLearnActive_ = false;
      nodeWakeStep_ = 0;
      nodeWakeNextActionMs_ = now;
      addEvent("cca learn timeout; continuing wake sequence");
      return false;
    }
    const uint16_t warmupSteps = ccaJoinBurstStepCount();
    if (nodeWakeLearnWarmupStep_ < warmupSteps) {
      if (nodeWakeLearnWarmupStep_ == 0) {
        addEvent("cca learn join burst start repeats=%u", (unsigned)TIGO_CCA_JOIN_BURST_REPEATS);
      }
      if (requestCcaJoinBurstStep(nodeWakeLearnWarmupStep_)) {
        ++nodeWakeLearnWarmupStep_;
        nodeWakeNextActionMs_ = now + TIGO_CCA_NODE_WAKE_SETUP_GAP_MS;
        if (nodeWakeLearnWarmupStep_ >= warmupSteps) {
          nodeWakeLearnLastJoinSeedMs_ = now;
          nodeWakeLearnLastNetworkStatusMs_ = now;
          addEvent("cca learn join burst complete; waiting for network");
        }
      }
      return true;
    }
    if (TIGO_SEND_41_DURING_STARTUP && radioJoinSeedMatchesCurrentProfile() &&
        (nodeWakeLearnLastJoinSeedMs_ == 0 ||
         elapsed(now, nodeWakeLearnLastJoinSeedMs_) >= TIGO_CCA_LEARN_JOIN_SEED_EVERY_MS)) {
      if (requestGatewayJoinSeed()) {
        nodeWakeLearnLastJoinSeedMs_ = now;
      }
      return true;
    }
    if (nodeWakeLearnLastNetworkStatusMs_ == 0 ||
        elapsed(now, nodeWakeLearnLastNetworkStatusMs_) >= TIGO_CCA_LEARN_NETWORK_STATUS_EVERY_MS) {
      if (requestNetworkStatus()) {
        nodeWakeLearnLastNetworkStatusMs_ = now;
      }
      return true;
    }
    return true;
  }

  void processNodeWakeSequence(uint32_t now) {
    if (!TIGO_CCA_NODE_WAKE_SEQUENCE || !activePollingEnabled_ || replayExclusiveMode_ || gatewayId_ == 0 ||
        tapCommandActive() || awaitingReceiveResponse_) {
      if (!activePollingEnabled_ && nodeWakeActive_) {
        finishNodeWakeSequence(false, "polling disabled");
      }
      return;
    }
    if (readOnlyWarmAttachProtectsState_ && !recoveryAuthorized_) {
      return;
    }
    if (TIGO_CCA_COMPAT_BOOT_SEQUENCE && bootCcaStep_ != UINT8_MAX) {
      return;
    }
    const uint16_t nodeCount = countWakeCandidateNodes();
    if (!nodeWakeActive_) {
      const bool wakeRetryDue =
          lastNodeWakeAttemptMs_ == 0 ||
          elapsed(now, lastNodeWakeAttemptMs_) >= TIGO_CCA_NODE_WAKE_RETRY_EVERY_MS;
      const bool networkNeedsConfirmation =
          lastNetworkStatusValid_ && lastNetworkExpectedNodes_ > 0 &&
          lastNetworkConfirmedNodes_ < lastNetworkExpectedNodes_ &&
          lastNodeTableMs_ != 0 && lastNodeTableEntryCount_ > 0;
      if (networkNeedsConfirmation && wakeRetryDue) {
        beginLearnForExistingTable(now, "network status not fully confirmed");
        return;
      }
      const bool needsLearn =
          TIGO_CCA_LEARN_BEFORE_NODE_WAKE &&
          countPendingNodeMap() > 0 &&
          countConfirmedNodeMap() == 0 &&
          lastNetworkConfirmedNodes_ == 0 &&
          (lastNetworkExpectedNodes_ > 0 || countPanelMapLongAddrs() > 0);
      if (needsLearn && wakeRetryDue) {
        beginNodeWakeSequence(now);
        return;
      }
      if (nodeCount == 0) {
        if (TIGO_ENABLE_NODE_SEED &&
            countPanelMapLongAddrs() > 0 &&
            lastNodeTableMs_ != 0 &&
            (lastNodeTableStart_ == 0 || lastNodeTableStart_ == TIGO_NODE_ID_BASE) &&
            lastNodeTableEntryCount_ == 0 &&
            countValidNodeMap() == 0 &&
            tapResponsesRx_ > 0 &&
            (nodeSeedState_ != NodeSeedState::Failed ||
             lastNodeWakeAttemptMs_ == 0 ||
             elapsed(now, lastNodeWakeAttemptMs_) >= TIGO_CCA_NODE_WAKE_RETRY_EVERY_MS)) {
          beginNodeWakeSequence(now);
          return;
        }
        if ((lastNodeWakeAttemptMs_ == 0 ||
             elapsed(now, lastNodeWakeAttemptMs_) >= TIGO_CCA_NODE_WAKE_RETRY_EVERY_MS) &&
            requestNodeTable(0)) {
          lastNodeWakeAttemptMs_ = now;
        }
        return;
      }
      if ((!nodeWakeCompleted_ || countValidPower() == 0) && wakeRetryDue) {
        beginNodeWakeSequence(now);
      } else {
        return;
      }
    }
    if (timeBefore(now, nodeWakeNextActionMs_)) {
      return;
    }
    if (processNodeWakeLearnPhase(now)) {
      return;
    }
    if (processNodeSeedRecovery(now)) {
      return;
    }
    const bool learnStillIncomplete =
        lastNetworkExpectedNodes_ == 0 ||
        lastNetworkConfirmedNodes_ < lastNetworkExpectedNodes_;
    if (TIGO_WAIT_FOR_LEARN_COUNTDOWN_AFTER_SEED &&
        !nodeWakeSkipLearn_ &&
        !nodeWakeLearnActive_ &&
        lastNetworkStatusValid_ &&
        lastNetworkMode_ == 1 &&
        lastNetworkCountdown_ > 0 &&
        learnStillIncomplete) {
      nodeWakeLearnActive_ = true;
      nodeWakeLearnWaitCountdown_ = true;
      nodeWakeLearnStartedMs_ = now;
      nodeWakeLearnWarmupStep_ = ccaJoinBurstStepCount();
      nodeWakeLearnLastJoinSeedMs_ = 0;
      nodeWakeLearnLastNetworkStatusMs_ = 0;
      addEvent("cca learn countdown observed; waiting mode=%u countdown=%u",
               lastNetworkMode_,
               lastNetworkCountdown_);
      return;
    }
    if (nodeSeedState_ == NodeSeedState::Failed && countValidNodeMap() == 0) {
      finishNodeWakeSequence(false, "node seed failed");
      return;
    }
    if (nodeSeedState_ == NodeSeedState::Done && !TIGO_WAKE_AFTER_NODE_SEED) {
      finishNodeWakeSequence(true, "node seed complete; wake disabled");
      return;
    }

    bool started = false;
    constexpr uint16_t joinBurstSteps = ccaJoinBurstStepCount();
    constexpr uint16_t setupBlockSteps = (uint16_t)(joinBurstSteps + 9U);
    constexpr uint16_t setupBlockRepeats = 1;
    constexpr uint16_t setupSteps = setupBlockSteps * setupBlockRepeats;
    if (nodeWakeStep_ < setupSteps) {
      const uint16_t setupStep = (uint16_t)(nodeWakeStep_ % setupBlockSteps);
      if (setupStep < joinBurstSteps) {
        if (setupStep == 0) {
          addEvent("cca join burst start repeats=%u", (unsigned)TIGO_CCA_JOIN_BURST_REPEATS);
        }
        started = requestCcaJoinBurstStep(setupStep);
      } else {
        switch ((uint16_t)(setupStep - joinBurstSteps)) {
        case 0:
          started = requestGatewayRadioConfig(0x0000);
          break;
        case 1:
          started = requestGatewayRadioConfig(0x0001);
          break;
        case 2:
          started = requestPvRunState(true);
          break;
        case 3:
          started = requestNetworkStatus();
          break;
        case 4:
          started = requestNodeTable(0);
          break;
        case 5:
          started = requestCcaNodeWindow(TIGO_NODE_ID_BASE);
          break;
        case 6:
          started = requestNodeTable(12);
          break;
        case 7:
          // Preserve the closing 00 01 word from the working CCA sequence.
          // The public protocol corpus labels its low bit "PV off", but live
          // captures also use it immediately before normal optimizer traffic.
          started = requestPvRunState(false);
          break;
        case 8:
          started = sendRsdControlFrame(true, "cca setup RSD run");
          break;
        }
      }
    } else {
      if (!TIGO_SEND_PER_NODE_WAKE) {
        finishNodeWakeSequence(true, "per-node wake disabled");
        return;
      }
      if (countValidNodeMap() == 0) {
        finishNodeWakeSequence(false, "no verified node table for per-node wake");
        return;
      }

      constexpr uint16_t firstPassNodeSteps = 4;
      constexpr uint16_t extendedNodeSteps = 3;
      const uint16_t perNodeStep = (uint16_t)(nodeWakeStep_ - setupSteps);
      const uint16_t firstPassTotalSteps = (uint16_t)(nodeCount * firstPassNodeSteps);
      if (perNodeStep < firstPassTotalSteps) {
        const uint16_t nodeIndex = (uint16_t)(perNodeStep / firstPassNodeSteps);
        const uint16_t nodeAction = (uint16_t)(perNodeStep % firstPassNodeSteps);
        uint16_t nodeId = 0;
        if (!nodeIdByCcaWakeIndex(nodeIndex, &nodeId)) {
          finishNodeWakeSequence(true, "cca first-pass nodes probed");
          return;
        }
        switch (nodeAction) {
          case 0:
            if (nodeIndex == 0) {
              addEvent("cca node wake first pass start");
            }
            started = sendNodeTextCommand(nodeId, "^00Version", true);
            break;
          case 1:
            started = sendNodeOpcodeCommand(0x17, nodeId, 0x0F);
            break;
          case 2:
            started = sendNodeTextCommand(nodeId, "^00Info", true);
            break;
          case 3:
            started = sendNodeTextCommand(nodeId, "^00Smrt", true);
            break;
        }
      } else if (perNodeStep == firstPassTotalSteps) {
        addEvent("cca node wake first pass settle %lu ms",
                 (unsigned long)TIGO_CCA_AFTER_FIRST_WAKE_SETTLE_MS);
        ++nodeWakeStep_;
        nodeWakeNextActionMs_ = now + TIGO_CCA_AFTER_FIRST_WAKE_SETTLE_MS;
        return;
      } else {
        const uint16_t extendedBaseStep = (uint16_t)(firstPassTotalSteps + 1U);
        const uint16_t extendedStep = (uint16_t)(perNodeStep - extendedBaseStep);
        const uint16_t actualExtendedTotalSteps =
            TIGO_CCA_NODE_WAKE_EXTENDED_COMMANDS ? (uint16_t)(nodeCount * extendedNodeSteps) : 0;
        if (extendedStep < actualExtendedTotalSteps) {
          const uint16_t nodeIndex = (uint16_t)(extendedStep / extendedNodeSteps);
          const uint16_t nodeAction = (uint16_t)(extendedStep % extendedNodeSteps);
          uint16_t nodeId = 0;
          if (!nodeIdByCcaWakeIndex(nodeIndex, &nodeId)) {
            finishNodeWakeSequence(true, "cca extended nodes probed");
            return;
          }
          switch (nodeAction) {
            case 0:
              if (nodeIndex == 0) {
                addEvent("cca node wake extended pass start");
              }
              started = sendNodeTextCommand(nodeId, "^00Mppt", true);
              break;
            case 1:
              started = sendNodeExtendedSmrtCommand(nodeId);
              break;
            case 2:
              if (TIGO_CCA_NODE_WAKE_PV_CONFIG_FALLBACK) {
                if (!nodeWakeConfigFallbackActive_) {
                  nodeWakeConfigFallbackActive_ = true;
                  addEvent("cca node wake pv-config start");
                }
                started = setPvConfig(nodeId,
                                      TIGO_CCA_NODE_WAKE_PV_PERIOD_SLOTS,
                                      ccaPhaseSlotsForNode(nodeId, nodeIndex),
                                      0x31);
              } else {
                started = true;
              }
              break;
          }
        } else {
          const uint16_t finalStep = (uint16_t)(extendedStep - actualExtendedTotalSteps);
          switch (finalStep) {
            case 0:
              addEvent("cca post-config preamble start");
              started = sendSimpleFrameCommand(0x003A, nullptr, 0, 0x003B, "cca post network info", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
              break;
            case 1:
              started = requestVersion();
              break;
            case 2:
              started = sendSimpleFrameCommand(0x0E02, nullptr, 0, 0x0006, "cca post e02 probe", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
              break;
            case 3:
              started = sendSimpleFrameCommand(0x000E, nullptr, 0, 0x000F, "cca post status probe", TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS);
              break;
            case 4:
              started = sendRsdControlFrame(false, "cca post RSD stop");
              break;
            case 5:
              started = requestGatewayRadioConfig(0x0000);
              break;
            case 6:
              started = sendRsdControlFrame(true, "cca post RSD run");
              break;
            case 7:
              started = requestGatewayRadioConfig(0x0001);
              break;
            case 8:
              started = requestPvRunState(true);
              break;
            case 9:
              started = requestNetworkStatus();
              break;
            case 10:
              started = requestNodeTable(0);
              break;
            case 11:
              started = requestPvRunState(false);
              break;
            case 12:
              started = requestNodeTable(12);
              break;
            case 13:
              ++nodeWakeStep_;
              nodeWakeNextActionMs_ = now + TIGO_CCA_FIRST_NODE_WAKE_DELAY_MS;
              return;
            default:
              {
                constexpr uint16_t postNodeSteps = 4;
                const uint16_t postNodeStep = (uint16_t)(finalStep - 14U);
                constexpr uint16_t postRounds =
                    TIGO_CCA_POST_CONFIG_WAKE_ROUNDS == 0 ? 1 : TIGO_CCA_POST_CONFIG_WAKE_ROUNDS;
                const uint16_t postNodeTotalSteps = (uint16_t)(nodeCount * postNodeSteps * postRounds);
                if (postNodeStep >= postNodeTotalSteps) {
                  finishNodeWakeSequence(true, "cca nodewise wake complete");
                  return;
                }
                const uint16_t round = (uint16_t)(postNodeStep / (nodeCount * postNodeSteps));
                const uint16_t nodeIndex = (uint16_t)((postNodeStep / postNodeSteps) % nodeCount);
                const uint16_t nodeAction = (uint16_t)(postNodeStep % postNodeSteps);
                uint16_t nodeId = 0;
                if (!nodeIdByCcaWakeIndex(nodeIndex, &nodeId)) {
                  finishNodeWakeSequence(true, "cca post-config nodes probed");
                  return;
                }
                if ((postNodeStep % (nodeCount * postNodeSteps)) == 0) {
                  addEvent("cca post-config all-node trigger round %u/%u",
                           (unsigned)(round + 1U),
                           (unsigned)postRounds);
                }
                switch (nodeAction) {
                  case 0:
                    started = sendNodeTextCommand(nodeId, "^00Version", true);
                    break;
                  case 1:
                    started = sendNodeOpcodeCommand(0x17, nodeId, 0x0F);
                    break;
                  case 2:
                    started = sendNodeTextCommand(nodeId, "^00Info", true);
                    break;
                  case 3:
                    started = sendNodeTextCommand(nodeId, "^00Smrt", true);
                    break;
                }
              }
              break;
          }
        }
      }
    }

    if (!started) {
      return;
    }
    ++nodeWakeStep_;
    if (nodeWakeStep_ == setupSteps) {
      nodeWakeNextActionMs_ = now + TIGO_CCA_FIRST_NODE_WAKE_DELAY_MS;
    } else {
      nodeWakeNextActionMs_ = now +
          (nodeWakeStep_ < setupSteps ? TIGO_CCA_NODE_WAKE_SETUP_GAP_MS : TIGO_CCA_NODE_WAKE_STEP_GAP_MS);
    }
  }

  void processTapCommand(uint32_t now) {
    if (!tapCommandActive()) {
      return;
    }

    if (commandState_.kind == TapCommandKind::Enumerate &&
        commandState_.phase == TapCommandPhase::EnumerateStartBurst &&
        timeReached(now, commandState_.nextActionMs)) {
      if (commandState_.attempt < 5 &&
          elapsed(now, commandState_.nextActionMs) < TIGO_ENUM_START_BURST_INTERVAL_MS &&
          commandState_.attempt > 0) {
        return;
      }
      static const uint8_t enumMagic[] = {0x37, 0x24, 0x92, 0x66};
      uint8_t startPayload[6];
      memcpy(startPayload, enumMagic, sizeof(enumMagic));
      startPayload[4] = (uint8_t)(commandState_.arg0 >> 8);
      startPayload[5] = (uint8_t)(commandState_.arg0 & 0xFF);
      if (commandState_.attempt < 5) {
        if (!sendGatewayFrame(0x0000, 0x0014, startPayload, sizeof(startPayload))) {
          failTapCommand("enumeration start blocked before transmit");
          return;
        }
        commandState_.attempt++;
        commandState_.nextActionMs = now;
        return;
      }
      sendGatewayFrame(commandState_.arg0, 0x0038, nullptr, 0);
      setTapCommandWait(TapCommandPhase::EnumerateWaitInfo,
                        0x0039,
                        commandState_.arg0,
                        TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                        "waiting for enumeration info");
      return;
    }

    if ((commandState_.phase == TapCommandPhase::WaitingFrame ||
         commandState_.phase == TapCommandPhase::EnumerateWaitInfo ||
         commandState_.phase == TapCommandPhase::EnumerateWaitAssign ||
         commandState_.phase == TapCommandPhase::EnumerateWaitVerify ||
         commandState_.phase == TapCommandPhase::AddressWaitVerify) &&
        timeReached(now, commandState_.deadlineMs)) {
      failTapCommand("timeout");
    }
  }

  bool decodeActivePvEnvelope(const uint8_t* payload, size_t payloadLen, ActivePvEnvelope& out) const {
    memset(&out, 0, sizeof(out));
    if (payload == nullptr || payloadLen < 5) {
      return false;
    }
    if (payload[0] != 0x00 || payload[2] != 0x00) {
      return false;
    }
    out.valid = true;
    out.txBuffersFree = payload[1];
    out.subcmd = payload[3];
    out.dsn = payload[4];
    out.body = payload + 5;
    out.bodyLen = payloadLen - 5;
    return true;
  }

  bool advertisedGatewayIdPlausible(uint16_t advertisedGatewayId,
                                    uint16_t frameGatewayId) const {
    if (advertisedGatewayId == 0) {
      return false;
    }
    const uint16_t candidates[] = {
      frameGatewayId,
      gatewayId_,
      persistedGatewayId_,
      bootJournal_.lastWorkingGatewayId,
      bootJournal_.transactionBeforeId,
      bootJournal_.transactionRequestedId,
      TIGO_DESIRED_GATEWAY_ID,
      TIGO_ENUM_ID,
      0x120A,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
      if (candidates[i] != 0 && advertisedGatewayId == candidates[i]) {
        return true;
      }
    }
    return false;
  }

  const char* expectedTapLongAddress() const {
    uint8_t ignored[8];
    if (hex16ToBytes(bootJournal_.tapLongAddr, ignored)) {
      return bootJournal_.tapLongAddr;
    }
    if (hex16ToBytes(gatewayLongAddr_, ignored)) {
      return gatewayLongAddr_;
    }
    return nullptr;
  }

  bool learnTapAddressFromEnumInfo(const uint8_t* payload, size_t payloadLen,
                                   const char* source,
                                   uint16_t frameGatewayId = 0) {
    if (payload == nullptr || payloadLen < 10) {
      return false;
    }
    char learnedLongAddr[17];
    bytesToHex(payload, 8, learnedLongAddr, sizeof(learnedLongAddr));
    uint8_t longAddrBytes[8];
    if (!hex16ToBytes(learnedLongAddr, longAddrBytes)) {
      return false;
    }
    const uint16_t advertisedGatewayId = ((uint16_t)payload[8] << 8) | payload[9];
    const char* expectedLongAddr = expectedTapLongAddress();
    if (expectedLongAddr != nullptr && strcmp(expectedLongAddr, learnedLongAddr) != 0) {
      addEvent("tap identity mismatch from %s expected=%s observed=%s; frame rejected",
               source ? source : "info", expectedLongAddr, learnedLongAddr);
      return false;
    }
    if (!advertisedGatewayIdPlausible(advertisedGatewayId, frameGatewayId)) {
      addEvent("tap advertised unexpected id from %s frame=%04X advertised=%04X; frame rejected",
               source ? source : "info", frameGatewayId, advertisedGatewayId);
      return false;
    }
    const bool changed = strcmp(gatewayLongAddr_, learnedLongAddr) != 0;
    if (changed && currentRadioDescriptorValid_ &&
        currentRadioDescriptorTapLongAddr_[0] != '\0' &&
        strcmp(currentRadioDescriptorTapLongAddr_, learnedLongAddr) != 0) {
      currentRadioDescriptorValid_ = false;
      currentRadioDescriptorTapLongAddr_[0] = '\0';
      addEvent("tap changed; current radio profile invalidated until readback");
    }
    copyString(gatewayLongAddr_, sizeof(gatewayLongAddr_), learnedLongAddr);
    if (currentRadioDescriptorValid_ && currentRadioDescriptorTapLongAddr_[0] == '\0') {
      copyString(currentRadioDescriptorTapLongAddr_, sizeof(currentRadioDescriptorTapLongAddr_), learnedLongAddr);
    }
    if (changed) {
      addEvent("tap long address learned from %s: %s current_id=%04X",
               source ? source : "info",
               gatewayLongAddr_,
               advertisedGatewayId);
    } else {
      addEvent("tap long address confirmed from %s: %s current_id=%04X",
               source ? source : "info",
               gatewayLongAddr_,
               advertisedGatewayId);
    }
    markPersistentStateDirty();
    copyString(bootJournal_.tapLongAddr, sizeof(bootJournal_.tapLongAddr), gatewayLongAddr_);
    if (frameGatewayId != 0) {
      bootJournal_.lastWorkingGatewayId = frameGatewayId;
    } else if (advertisedGatewayId != 0) {
      bootJournal_.lastWorkingGatewayId = advertisedGatewayId;
    }
    markBootJournalDirty();
    return true;
  }

  bool handleTapCommandFrame(const GatewayFrame& frame) {
    if (!tapCommandActive()) {
      return false;
    }
    const bool shortPvAck =
        frame.typeCode == 0x0146 &&
        frame.fromGateway &&
        frame.gatewayId == commandState_.expectedGatewayId &&
        (commandState_.kind == TapCommandKind::PvConfig ||
         commandState_.kind == TapCommandKind::PvSubcommand);
    if (shortPvAck) {
      lastCommandResponseType_ = frame.typeCode;
      lastCommandResponseGatewayId_ = frame.gatewayId;
      bytesToHex(frame.payload, frame.payloadLen, commandState_.ackHex, sizeof(commandState_.ackHex));
      const bool acceptShortNodeTextAck =
          TIGO_ACCEPT_SHORT_NODE_TEXT_ACK &&
          commandState_.kind == TapCommandKind::PvSubcommand &&
          (commandState_.arg0 & 0xFFU) == 0x06 &&
          frame.payloadLen == 2 &&
          frame.payload[0] == 0x00 &&
          frame.payload[1] == 0x02;
      if (acceptShortNodeTextAck) {
        lastPvSubcommand_ = 0x06;
        copyString(lastPvSubcommandAckHex_, sizeof(lastPvSubcommandAckHex_), commandState_.ackHex);
        lastPvAckStatusFlags_ = 0;
        lastPvAckResponseSubcmd_ = 0;
        lastPvAckBodyHex_[0] = '\0';
        // 0x0146/0002 is emitted by the TAP itself. It proves only that the
        // bus command was accepted locally; topology or 0x31 is RF evidence.
        addEvent("pv-subcmd 0x06 local TAP ack raw=%s; RF unconfirmed", commandState_.ackHex);
        finishTapCommand(true, "pv subcommand local TAP ack; RF unconfirmed");
        return true;
      }
      if (commandState_.kind == TapCommandKind::PvSubcommand) {
        addEvent("pv-subcmd 0x%02X short ack raw=%s; waiting active ack",
                 (unsigned)(commandState_.arg0 & 0xFFU),
                 commandState_.ackHex);
      } else {
        addEvent("pv-config node=%u short ack raw=%s; waiting active ack",
                 commandState_.arg0,
                 commandState_.ackHex);
      }
      return true;
    }
    if (!frame.fromGateway || frame.typeCode != commandState_.expectedType ||
        frame.gatewayId != commandState_.expectedGatewayId) {
      return false;
    }
    if (frame.typeCode == 0x0B10) {
      ActivePvEnvelope env{};
      if (!decodeActivePvEnvelope(frame.payload, frame.payloadLen, env) ||
          env.dsn != commandState_.expectedDsn) {
        return false;
      }
      lastCommandResponseType_ = frame.typeCode;
      lastCommandResponseGatewayId_ = frame.gatewayId;
      lastCommandResponseDsn_ = env.dsn;
      if (commandState_.expectedPvResponseSubcmd != 0 &&
          env.subcmd != commandState_.expectedPvResponseSubcmd) {
        addEvent("pv ack dsn=%u unexpected rsp=0x%02X expected=0x%02X",
                 env.dsn, env.subcmd, commandState_.expectedPvResponseSubcmd);
        failTapCommand("unexpected pv ack subcommand");
        return true;
      }
      // A matching active envelope is a TAP-level command result. Do not turn
      // it into a false optimizer success: only a received topology/0x31 RF
      // frame proves that the addressed optimizer is actually reachable.
    } else {
      lastCommandResponseType_ = frame.typeCode;
      lastCommandResponseGatewayId_ = frame.gatewayId;
      lastCommandResponseDsn_ = 0;
    }

    if ((frame.typeCode == 0x0039 || frame.typeCode == 0x003B) &&
        !learnTapAddressFromEnumInfo(frame.payload, frame.payloadLen,
                                     frame.typeCode == 0x0039
                                         ? "0039-command-verification"
                                         : "003B-command-verification",
                                     frame.gatewayId)) {
      failTapCommand("TAP identity or advertised address verification failed");
      return true;
    }

    switch (commandState_.kind) {
      case TapCommandKind::Ping:
        finishTapCommand(true, "read-only network probe ack");
        return true;
      case TapCommandKind::RsdControl:
        lastRsdControlKnown_ = true;
        lastRsdRunState_ = commandState_.arg3 == 0x01;
        lastRsdControlAckMs_ = platformMillis();
        if (!lastRsdRunState_) {
          firstReleasedPowerEvidenceMs_ = 0;
          lastReleasedPowerEvidenceMs_ = 0;
          releasedPowerEvidenceCount_ = 0;
        }
        statusDirty_ = true;
        addEvent("RSD control acknowledged: %s", lastRsdRunState_ ? "RUN" : "STOP");
        finishTapCommand(true, lastRsdRunState_ ? "RSD RUN ack" : "RSD STOP ack");
        return true;
      case TapCommandKind::Version: {
        char txt[96];
        size_t n = frame.payloadLen;
        if (n >= sizeof(txt)) {
          n = sizeof(txt) - 1;
        }
        memcpy(txt, frame.payload, n);
        txt[n] = '\0';
        sanitizeInlineText(txt);
        copyString(webVersionText_, sizeof(webVersionText_), txt);
        copyString(bootJournal_.tapFirmware, sizeof(bootJournal_.tapFirmware), txt);
        copyString(bootJournal_.tapLongAddr, sizeof(bootJournal_.tapLongAddr), gatewayLongAddr_);
        bootJournal_.lastWorkingGatewayId = gatewayId_;
        markBootJournalDirty();
        finishTapCommand(true, "version ack");
        return true;
      }
      case TapCommandKind::NodeTable: {
        ActivePvEnvelope env{};
        if (!decodeActivePvEnvelope(frame.payload, frame.payloadLen, env) ||
            env.subcmd != 0x27 || env.bodyLen < 4) {
          failTapCommand("malformed node table response");
          return true;
        }
        const uint16_t responseEntries = ((uint16_t)env.body[2] << 8) | env.body[3];
        if (env.bodyLen != 4U + ((size_t)responseEntries * 10U)) {
          failTapCommand("truncated node table response");
          return true;
        }
        bytesToHex(frame.payload, frame.payloadLen, lastNodeTableAckHex_, sizeof(lastNodeTableAckHex_));
        const bool learnExistingAfterProfileWrite =
            forceLearnAfterProfileWrite_ && lastNodeTableMs_ != 0 && lastNodeTableEntryCount_ > 0;
        const bool startSeedAfterEmptyTable =
            TIGO_ENABLE_NODE_SEED &&
            nodeWakeActive_ &&
            !nodeSeedAwaitingCommand_ &&
            nodeSeedState_ != NodeSeedState::ClearTable &&
            nodeSeedState_ != NodeSeedState::SendChunk &&
            nodeSeedState_ != NodeSeedState::VerifyNodeTable &&
            nodeSeedState_ != NodeSeedState::StartLearn &&
            nodeSeedState_ != NodeSeedState::EnablePv &&
            (lastNodeTableStart_ == 0 || lastNodeTableStart_ == TIGO_NODE_ID_BASE) &&
            lastNodeTableEntryCount_ == 0 &&
            countPanelMapLongAddrs() > 0 &&
            (!lastNetworkStatusValid_ || lastNetworkExpectedNodes_ == 0);
        finishTapCommand(true, "node table ack");
        if (learnExistingAfterProfileWrite) {
          forceLearnAfterProfileWrite_ = false;
          beginLearnForExistingTable(platformMillis(), "radio profile changed");
        } else if (startSeedAfterEmptyTable) {
          addEvent("empty initial node table; starting node seed recovery");
          beginNodeSeedRecovery(platformMillis());
        }
        return true;
      }
      case TapCommandKind::NetworkStatus: {
        ActivePvEnvelope env{};
        if (!decodeActivePvEnvelope(frame.payload, frame.payloadLen, env) ||
            env.subcmd != 0x2F || env.bodyLen < 9) {
          failTapCommand("malformed network status response");
          return true;
        }
        bytesToHex(frame.payload, frame.payloadLen, lastNetworkStatusAckHex_, sizeof(lastNetworkStatusAckHex_));
        finishTapCommand(true, "network status ack");
        return true;
      }
      case TapCommandKind::RadioConfig: {
        if (commandState_.arg0 == 2) {
          requestRadioProfileReadbackForStore();
          return true;
        }
        if (commandState_.arg0 == 4) {
          acknowledgeAddressTransaction();
          commandState_.arg0 = 6;
          if (!sendGatewayFrame(gatewayId_, 0x003A, nullptr, 0)) {
            failTapCommand("radio STORE verification probe blocked before transmit");
            return true;
          }
          setTapCommandWait(TapCommandPhase::AddressWaitVerify,
                            0x003B,
                            gatewayId_,
                            TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                            "verifying radio STORE address");
          return true;
        }
        if (commandState_.arg0 == 6) {
          verifyAddressTransaction(frame.gatewayId);
          requestGatewayHardResetAfterRadioStore();
          return true;
        }
        if (commandState_.arg0 == 5) {
          acknowledgeAddressTransaction();
          commandState_.arg0 = 7;
          if (!sendGatewayFrame(gatewayId_, 0x003A, nullptr, 0)) {
            failTapCommand("radio APPLY verification probe blocked before transmit");
            return true;
          }
          setTapCommandWait(TapCommandPhase::AddressWaitVerify,
                            0x003B,
                            gatewayId_,
                            TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                            "verifying radio APPLY address");
          return true;
        }
        if (commandState_.arg0 == 7) {
          verifyAddressTransaction(frame.gatewayId);
          // Preserve the last observed table until a read-only 0x26 response
          // proves that the TAP actually changed it.
          forceLearnAfterProfileWrite_ = true;
          bootNodeTablePending_ = true;
          addEvent("radio profile persisted through verified STORE/APPLY; node table preserved pending readback");
          finishTapCommand(true, "radio profile write persisted");
          return true;
        }
        ActivePvEnvelope env{};
        if (!decodeActivePvEnvelope(frame.payload, frame.payloadLen, env) ||
            env.subcmd != 0x0E ||
            (env.bodyLen != TIGO_RADIO_DESCRIPTOR_LEN &&
             env.bodyLen != TIGO_RADIO_DESCRIPTOR_LEN + 1U) ||
            !radioDescriptorLooksValid(env.body)) {
          failTapCommand("radio profile read/write rejected");
          return true;
        }
        const uint8_t result =
            env.bodyLen == TIGO_RADIO_DESCRIPTOR_LEN + 1U
                ? env.body[TIGO_RADIO_DESCRIPTOR_LEN]
                : 0x00;
        if ((commandState_.arg0 == 1 || commandState_.arg0 == 3) &&
            (!commandState_.expectedRadioDescriptorValid ||
             memcmp(env.body, commandState_.expectedRadioDescriptor,
                    TIGO_RADIO_DESCRIPTOR_LEN) != 0)) {
          failTapCommand("radio profile readback differs from requested descriptor");
          return true;
        }
        bytesToHex(frame.payload, frame.payloadLen, lastRadioConfigAckHex_, sizeof(lastRadioConfigAckHex_));
        if (commandState_.arg0 == 1) {
          if (result != 0x00) {
            failTapCommand(result == 0x01
                               ? "radio profile write deferred while learn is active"
                               : "radio profile write rejected");
            return true;
          }
          static const uint8_t runPayload[] = {0x01};
          commandState_.arg0 = 2;
          ++destructiveManagementFramesTx_;
          if (!sendGatewayFrame(gatewayId_, 0x0B00, runPayload, sizeof(runPayload))) {
            failTapCommand("radio profile RSD RUN blocked before transmit");
            return true;
          }
          setTapCommandWait(TapCommandPhase::WaitingFrame, 0x0B01, gatewayId_, 800,
                            "waiting for radio profile RSD RUN ack");
          addEvent("radio profile write accepted; issuing RSD RUN before STORE/APPLY");
          return true;
        }
        if (commandState_.arg0 == 3) {
          if (result != 0x00) {
            failTapCommand(result == 0x01
                               ? "radio profile readback busy while learn is active"
                               : "radio profile readback rejected");
            return true;
          }
          requestRadioProfileStore();
          return true;
        }
        if (result == 0x01) {
          addEvent("radio profile snapshot returned busy result=01 during learn");
          finishTapCommand(true, "radio config busy snapshot");
          return true;
        }
        if (result != 0x00) {
          failTapCommand("radio profile read rejected");
          return true;
        }
        finishTapCommand(true, "radio config ack");
        return true;
      }
      case TapCommandKind::PvConfig:
        bytesToHex(frame.payload, frame.payloadLen, commandState_.ackHex, sizeof(commandState_.ackHex));
        addEvent("pv-config node=%u period=%u phase=%u TAP ack; RF unconfirmed",
                 commandState_.arg0, commandState_.arg1, commandState_.arg2);
        finishTapCommand(true, "pv config TAP ack; RF unconfirmed");
        return true;
      case TapCommandKind::PvSubcommand:
        lastPvSubcommand_ = (uint8_t)(commandState_.arg0 & 0xFFU);
        bytesToHex(frame.payload, frame.payloadLen, commandState_.ackHex, sizeof(commandState_.ackHex));
        copyString(lastPvSubcommandAckHex_, sizeof(lastPvSubcommandAckHex_), commandState_.ackHex);
        {
          ActivePvEnvelope env;
          if (decodeActivePvEnvelope(frame.payload, frame.payloadLen, env)) {
            lastPvAckStatusFlags_ = env.txBuffersFree;
            lastPvAckResponseSubcmd_ = env.subcmd;
            bytesToHex(env.body, env.bodyLen, lastPvAckBodyHex_, sizeof(lastPvAckBodyHex_));
            if (commandState_.expectedPvResponseSubcmd != 0 &&
                env.subcmd != commandState_.expectedPvResponseSubcmd) {
              addEvent("pv-subcmd 0x%02X unexpected rsp=0x%02X expected=0x%02X",
                       (unsigned)lastPvSubcommand_,
                       (unsigned)env.subcmd,
                       (unsigned)commandState_.expectedPvResponseSubcmd);
              failTapCommand("unexpected pv ack subcommand");
              return true;
            }
            addEvent("pv-subcmd 0x%02X TAP ack status=0x%02X rsp=0x%02X body=%s; RF unconfirmed",
                     (unsigned)lastPvSubcommand_,
                     (unsigned)lastPvAckStatusFlags_,
                     (unsigned)lastPvAckResponseSubcmd_,
                     lastPvAckBodyHex_);
          } else {
            lastPvAckStatusFlags_ = 0;
            lastPvAckResponseSubcmd_ = 0;
            lastPvAckBodyHex_[0] = '\0';
            addEvent("pv-subcmd 0x%02X TAP ack raw=%s; RF unconfirmed",
                     (unsigned)lastPvSubcommand_, lastPvSubcommandAckHex_);
          }
        }
        finishTapCommand(true, "pv subcommand TAP ack; RF unconfirmed");
        return true;
      case TapCommandKind::SimpleFrame:
        if (commandState_.arg0 == 0x0010 || commandState_.arg0 == 0x0012) {
          if (commandState_.phase != TapCommandPhase::AddressWaitVerify) {
            acknowledgeAddressTransaction();
            if (commandState_.arg3 == 0) {
              finishTapCommand(true, "address handshake acknowledged; replay verification pending");
              return true;
            }
            const uint16_t verificationId = commandState_.arg2 != 0
                ? commandState_.arg2 : gatewayId_;
            if (!sendGatewayFrame(verificationId, 0x003A, nullptr, 0)) {
              failTapCommand("address verification probe blocked before transmit");
              return true;
            }
            setTapCommandWait(TapCommandPhase::AddressWaitVerify,
                              0x003B,
                              verificationId,
                              TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                              "verifying address handshake result");
            return true;
          }
          gatewayId_ = frame.gatewayId;
          markPersistentStateDirty();
          if (!flushPersistentState(true)) {
            failAddressTransaction();
            failTapCommand("verified address could not be persisted");
            return true;
          }
          verifyAddressTransaction(gatewayId_);
          finishTapCommand(true, "address handshake verified");
          return true;
        }
        if (commandState_.arg0 == 0x0B00 && commandState_.arg3 <= 0x01) {
          lastRsdControlKnown_ = true;
          lastRsdRunState_ = commandState_.arg3 == 0x01;
          lastRsdControlAckMs_ = platformMillis();
          if (!lastRsdRunState_) {
            firstReleasedPowerEvidenceMs_ = 0;
            lastReleasedPowerEvidenceMs_ = 0;
            releasedPowerEvidenceCount_ = 0;
          }
          statusDirty_ = true;
          addEvent("RSD control acknowledged through sequence: %s",
                   lastRsdRunState_ ? "RUN" : "STOP");
        }
        if (commandState_.arg0 == 0x0038 && frame.typeCode == 0x0039) {
          learnTapAddressFromEnumInfo(frame.payload, frame.payloadLen, "0039",
                                      frame.gatewayId);
        }
        if (bootCcaWaitingStep_ == 13 && commandState_.arg0 == 0x003A &&
            frame.typeCode == 0x003B && frame.gatewayId != gatewayId_) {
          addEvent("cca final network info answered on %04X; switching active gateway from %04X",
                   frame.gatewayId,
                   gatewayId_);
          gatewayId_ = frame.gatewayId;
          markPersistentStateDirty();
        }
        finishTapCommand(true, "simple frame ack");
        return true;
      case TapCommandKind::BootReceiveSeed:
        lastTapResponseMs_ = platformMillis();
        if (!processReceiveResponse(frame)) {
          failTapCommand("invalid boot receive seed response");
          return true;
        }
        ++tapResponsesRx_;
        finishTapCommand(true, "validated boot receive seed ack");
        return true;
      case TapCommandKind::Enumerate: {
        if (commandState_.phase == TapCommandPhase::EnumerateWaitInfo) {
          if (frame.payloadLen < 10) {
            failTapCommand("enumeration response too short");
            return true;
          }
          bytesToHex(frame.payload, 8, commandState_.discoveredLongAddr, sizeof(commandState_.discoveredLongAddr));
          const uint16_t currentId = ((uint16_t)frame.payload[8] << 8) | frame.payload[9];
          if (commandState_.arg1 != 0 && commandState_.arg1 != currentId) {
            static const uint8_t enumMagic[] = {0x37, 0x24, 0x92, 0x66};
            uint8_t assignPayload[14];
            memcpy(assignPayload, enumMagic, sizeof(enumMagic));
            memcpy(assignPayload + 4, frame.payload, 8);
            assignPayload[12] = (uint8_t)(commandState_.arg1 >> 8);
            assignPayload[13] = (uint8_t)(commandState_.arg1 & 0xFF);
            beginAddressTransaction(0x003C, currentId, commandState_.arg1,
                                    "enumerated address assignment");
            if (!flushBootJournal(true)) {
              failTapCommand("enumeration assignment journal flush failed");
              return true;
            }
            if (!sendGatewayFrame(commandState_.arg0, 0x003C,
                                  assignPayload, sizeof(assignPayload))) {
              failTapCommand("enumeration assign blocked before transmit");
              return true;
            }
            setTapCommandWait(TapCommandPhase::EnumerateWaitAssign,
                              0x003D,
                              commandState_.arg0,
                              TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                              "waiting for enumeration assign ack");
            return true;
          }
          gatewayId_ = currentId;
          copyString(gatewayLongAddr_, sizeof(gatewayLongAddr_), commandState_.discoveredLongAddr);
          markPersistentStateDirty();
          if (!flushPersistentState(true)) {
            failAddressTransaction();
            failTapCommand("enumerated address could not be persisted");
            return true;
          }
          verifyAddressTransaction(gatewayId_);
          finishTapCommand(true, "enumeration verified existing address");
          return true;
        }
        if (commandState_.phase == TapCommandPhase::EnumerateWaitAssign) {
          acknowledgeAddressTransaction();
          if (!sendGatewayFrame(commandState_.arg1, 0x003A, nullptr, 0)) {
            failTapCommand("enumeration verification probe blocked before transmit");
            return true;
          }
          setTapCommandWait(TapCommandPhase::EnumerateWaitVerify,
                            0x003B,
                            commandState_.arg1,
                            TIGO_CCA_SIMPLE_FRAME_TIMEOUT_MS,
                            "verifying enumerated address");
          return true;
        }
        if (commandState_.phase == TapCommandPhase::EnumerateWaitVerify) {
          gatewayId_ = commandState_.arg1;
          copyString(gatewayLongAddr_, sizeof(gatewayLongAddr_), commandState_.discoveredLongAddr);
          markPersistentStateDirty();
          if (!flushPersistentState(true)) {
            failAddressTransaction();
            failTapCommand("verified enumerated address could not be persisted");
            return true;
          }
          verifyAddressTransaction(gatewayId_);
          finishTapCommand(true, "enumeration assignment verified");
          return true;
        }
        return false;
      }
      case TapCommandKind::None:
      default:
        return false;
    }
  }

  bool queueTapCommandResponse(const char* commandName, bool ok, const char* message) {
    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"queued\":");
    body += ok ? F("true") : F("false");
    body += F(",\"command\":\"");
    body += commandName;
    body += F("\",\"message\":\"");
    body += jsonEscape(message);
    body += F("\"}");
    sendJson(body);
    return ok;
  }

  // --------------------------
  // Event log
  // --------------------------
  void addEvent(const char* fmt, ...) {
    char tmp[EVENT_TEXT_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    Serial.printf("OTX event t=%lu %s\r\n", (unsigned long)platformMillis(), tmp);
    events_[recentEventHead_].ms = platformMillis();
    statusDirty_ = true;
    copyString(events_[recentEventHead_].text, sizeof(events_[recentEventHead_].text), tmp);
    recentEventHead_ = (recentEventHead_ + 1U) % MAX_EVENTS;
  }

  // --------------------------
  // State persistence
  // --------------------------
  void markPersistentStateDirty(uint32_t now = 0) {
    persistentStateDirty_ = true;
    persistentStateDirtySinceMs_ = now ? now : platformMillis();
  }

  bool flushPersistentState(bool force, uint32_t now = 0) {
    if (!persistentStateDirty_) {
      return true;
    }
    const uint32_t effectiveNow = now ? now : platformMillis();
    if (!force && elapsed(effectiveNow, persistentStateDirtySinceMs_) < TIGO_PERSISTENT_STATE_SAVE_DEBOUNCE_MS) {
      return false;
    }
    if (!saveStateToPersistentStore()) {
      return false;
    }
    persistentStateDirty_ = false;
    lastStateSaveMs_ = effectiveNow;
    return true;
  }

  uint32_t bootOptionsChecksum(const BootOptionsSettings& value) const {
    BootOptionsSettings copy = value;
    copy.checksum = 0;
    return fnv1a32(reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
  }

  uint32_t bootJournalChecksum(const BootJournalSettings& value) const {
    BootJournalSettings copy = value;
    copy.checksum = 0;
    return fnv1a32(reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
  }

  void resetBootOptionsSettings() {
    memset(&bootOptions_, 0, sizeof(bootOptions_));
    bootOptions_.magic = TIGO_BOOT_OPTIONS_MAGIC;
    bootOptions_.version = TIGO_BOOT_OPTIONS_VERSION;
    bootOptions_.cursorStrategy = (uint8_t)BootCursorStrategy::Persisted;
    bootOptions_.checksum = bootOptionsChecksum(bootOptions_);
    bootCursorStrategy_ = BootCursorStrategy::Persisted;
  }

  void resetBootJournalSettings() {
    bootJournalLoaded_ = false;
    memset(&bootJournal_, 0, sizeof(bootJournal_));
    bootJournal_.magic = TIGO_BOOT_JOURNAL_MAGIC;
    bootJournal_.version = TIGO_BOOT_JOURNAL_VERSION;
    bootJournal_.lastWorkingGatewayId = TIGO_GATEWAY_ID;
    bootJournal_.packetCursor = TIGO_INITIAL_PACKET_NUMBER;
    bootJournal_.lastBootPath = (uint8_t)TapBootPath::Unknown;
    bootJournal_.lastTapState = (uint8_t)TapObservedState::UnknownUnreachable;
    bootJournal_.lastCursorStrategy = (uint8_t)BootCursorStrategy::Persisted;
    bootJournal_.transactionState = (uint8_t)AddressTransactionState::None;
    bootJournal_.checksum = bootJournalChecksum(bootJournal_);
  }

  void loadBootOptionsFromPersistentStore() {
    if (!persistentStoreReady_) {
      return;
    }
    BootOptionsSettings stored{};
    const size_t read = platformRuntime_.persistentStore().loadBytes(
        TIGO_PREFS_BOOT_OPTIONS_KEY, &stored, sizeof(stored));
    if (read != sizeof(stored) || stored.magic != TIGO_BOOT_OPTIONS_MAGIC ||
        stored.version != TIGO_BOOT_OPTIONS_VERSION ||
        stored.cursorStrategy > (uint8_t)BootCursorStrategy::CcaBootstrap ||
        stored.checksum != bootOptionsChecksum(stored)) {
      resetBootOptionsSettings();
      return;
    }
    bootOptions_ = stored;
    bootCursorStrategy_ = (BootCursorStrategy)stored.cursorStrategy;
  }

  bool saveBootOptionsToPersistentStore() {
    if (!persistentStoreReady_) {
      return false;
    }
    bootOptions_.magic = TIGO_BOOT_OPTIONS_MAGIC;
    bootOptions_.version = TIGO_BOOT_OPTIONS_VERSION;
    bootOptions_.cursorStrategy = (uint8_t)bootCursorStrategy_;
    bootOptions_.checksum = bootOptionsChecksum(bootOptions_);
    return platformRuntime_.persistentStore().saveBytes(
        TIGO_PREFS_BOOT_OPTIONS_KEY, &bootOptions_, sizeof(bootOptions_));
  }

  void loadBootJournalFromPersistentStore() {
    if (!persistentStoreReady_) {
      return;
    }
    BootJournalSettings stored{};
    const size_t read = platformRuntime_.persistentStore().loadBytes(
        TIGO_PREFS_BOOT_JOURNAL_KEY, &stored, sizeof(stored));
    if (read != sizeof(stored) || stored.magic != TIGO_BOOT_JOURNAL_MAGIC ||
        stored.version != TIGO_BOOT_JOURNAL_VERSION ||
        stored.checksum != bootJournalChecksum(stored) ||
        stored.tapLongAddr[sizeof(stored.tapLongAddr) - 1] != '\0' ||
        stored.tapFirmware[sizeof(stored.tapFirmware) - 1] != '\0' ||
        stored.lastMutation[sizeof(stored.lastMutation) - 1] != '\0') {
      resetBootJournalSettings();
      addEvent("boot journal missing or invalid; starting a fresh journal");
      return;
    }
    bootJournal_ = stored;
    bootJournalLoaded_ = true;
    addEvent("boot journal loaded: gateway=%04X cursor=%04X state=%s path=%s rollback=%s",
             bootJournal_.lastWorkingGatewayId,
             bootJournal_.packetCursor,
             tapObservedStateName((TapObservedState)bootJournal_.lastTapState),
             tapBootPathName((TapBootPath)bootJournal_.lastBootPath),
             bootJournal_.rollbackNeeded ? "yes" : "no");
  }

  void reconcilePersistentStateWithBootJournal() {
    if (!bootJournalLoaded_) {
      return;
    }
    if (persistedPacketCursor_ != bootJournal_.packetCursor) {
      const uint16_t stateCursor = persistedPacketCursor_;
      // The journal is written before the broader controller state. Trusting
      // it favors harmless duplicate delivery over skipping TAP queue data
      // after a power loss between the two NVS writes.
      persistedPacketCursor_ = bootJournal_.packetCursor;
      confirmedPacketCursor_ = persistedPacketCursor_;
      nextPacketNumber_ = persistedPacketCursor_;
      markPersistentStateDirty();
      if (!flushPersistentState(true)) {
        ++cursorCheckpointFailureCount_;
        addEvent("cursor reconciliation state write failed journal=%04X state=%04X",
                 bootJournal_.packetCursor, stateCursor);
      } else {
        addEvent("cursor reconciled from write-ahead journal: state=%04X journal=%04X",
                 stateCursor, bootJournal_.packetCursor);
      }
    }
    bool interruptedMutationFound = false;
    for (size_t i = 0; i < TIGO_BOOT_MUTATION_JOURNAL_ENTRIES; ++i) {
      BootMutationEntry& entry = bootJournal_.mutations[i];
      if (entry.typeCode != 0 && entry.result == 0) {
        entry.result = 3; // interrupted/unknown after restart
        entry.confirmedGatewayId = bootJournal_.lastWorkingGatewayId;
        entry.afterNetworkConfirmed = bootJournal_.networkConfirmed;
        entry.afterNodeTableHash = bootJournal_.nodeTableHash;
        interruptedMutationFound = true;
      }
    }
    if (interruptedMutationFound) {
      copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
                 "interrupted mutation recovered");
      markBootJournalDirty();
      if (!flushBootJournal(true)) {
        addEvent("interrupted mutation journal reconciliation failed");
      } else {
        addEvent("interrupted mutation recorded; address rollback=%s",
                 bootJournal_.rollbackNeeded ? "required" : "not-required");
      }
    }
  }

  void markBootJournalDirty(uint32_t now = 0) {
    bootJournalDirty_ = true;
    if (bootJournalDirtySinceMs_ == 0) {
      bootJournalDirtySinceMs_ = now ? now : platformMillis();
    }
  }

  bool flushBootJournal(bool force, uint32_t now = 0) {
    if (!bootJournalDirty_) {
      return true;
    }
    const uint32_t effectiveNow = now ? now : platformMillis();
    if (!force && elapsed(effectiveNow, bootJournalDirtySinceMs_) <
                      TIGO_PERSISTENT_STATE_SAVE_DEBOUNCE_MS) {
      return false;
    }
    if (!persistentStoreReady_) {
      return false;
    }
    bootJournal_.magic = TIGO_BOOT_JOURNAL_MAGIC;
    bootJournal_.version = TIGO_BOOT_JOURNAL_VERSION;
    bootJournal_.checksum = bootJournalChecksum(bootJournal_);
    if (!platformRuntime_.persistentStore().saveBytes(
            TIGO_PREFS_BOOT_JOURNAL_KEY, &bootJournal_, sizeof(bootJournal_))) {
      return false;
    }
    bootJournalDirty_ = false;
    bootJournalDirtySinceMs_ = 0;
    return true;
  }

  void applyBootCursorStrategy() {
    switch (bootCursorStrategy_) {
      case BootCursorStrategy::Zero:
      case BootCursorStrategy::CcaBootstrap:
        nextPacketNumber_ = 0;
        break;
      case BootCursorStrategy::Persisted:
      default:
        nextPacketNumber_ = persistedPacketCursor_;
        break;
    }
    lastRequestedPacketNumber_ = nextPacketNumber_;
    addEvent("boot cursor strategy=%s persisted=%04X initial_request=%04X",
             bootCursorStrategyName(bootCursorStrategy_),
             persistedPacketCursor_, nextPacketNumber_);
  }

  void checkpointConfirmedCursor(uint16_t cursor, uint32_t now) {
    const bool firstConfirmationThisBoot = !cursorConfirmedThisBoot_;
    confirmedPacketCursor_ = cursor;
    cursorConfirmedThisBoot_ = true;
    if (cursorEpochResetPending_) {
      const uint16_t previous = persistedPacketCursor_;
      cursorEpochResetPending_ = false;
      lastCursorCheckpointMs_ = now;
      if (persistConfirmedCursor(cursor, now)) {
        addEvent("cursor epoch reset confirmed by valid 0x0149: %04X -> %04X",
                 previous, cursor);
      }
      return;
    }
    const uint16_t advanceFromPersisted = (uint16_t)(cursor - persistedPacketCursor_);
    const bool atOrAheadOfPersisted =
        cursor == persistedPacketCursor_ || advanceFromPersisted < 0x8000U;
    if (!atOrAheadOfPersisted) {
      ++cursorPersistenceHoldCount_;
      if (cursorPersistenceHoldCount_ == 1) {
        addEvent("cursor rewind confirmed in RAM=%04X; preserving persisted high-water=%04X",
                 cursor, persistedPacketCursor_);
      }
      return;
    }
    if (firstConfirmationThisBoot || lastCursorCheckpointMs_ == 0 ||
        elapsed(now, lastCursorCheckpointMs_) >= TIGO_CURSOR_CHECKPOINT_EVERY_MS) {
      lastCursorCheckpointMs_ = now;
      persistConfirmedCursor(cursor, now);
    }
  }

  bool persistConfirmedCursor(uint16_t cursor, uint32_t now) {
    const uint16_t oldJournalCursor = bootJournal_.packetCursor;
    bootJournal_.packetCursor = cursor;
    markBootJournalDirty(now);
    if (!flushBootJournal(true, now)) {
      bootJournal_.packetCursor = oldJournalCursor;
      markBootJournalDirty(now);
      ++cursorCheckpointFailureCount_;
      addEvent("cursor journal checkpoint failed; keeping persisted=%04X confirmed=%04X",
               persistedPacketCursor_, cursor);
      return false;
    }

    persistedPacketCursor_ = cursor;
    markPersistentStateDirty(now);
    if (!flushPersistentState(true, now)) {
      ++cursorCheckpointFailureCount_;
      addEvent("cursor state checkpoint failed after journal commit=%04X; retry queued",
               cursor);
      return false;
    }
    return true;
  }

  void beginAddressTransaction(uint16_t typeCode, uint16_t beforeId,
                               uint16_t requestedId, const char* label) {
    bootJournal_.transactionType = typeCode;
    bootJournal_.transactionBeforeId = beforeId;
    bootJournal_.transactionRequestedId = requestedId;
    bootJournal_.transactionConfirmedId = 0;
    bootJournal_.transactionState = (uint8_t)AddressTransactionState::Requested;
    bootJournal_.rollbackNeeded = 1;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation), label ? label : "address transaction");
    markBootJournalDirty();
    flushBootJournal(true);
  }

  void acknowledgeAddressTransaction() {
    if (bootJournal_.transactionState == (uint8_t)AddressTransactionState::Requested) {
      bootJournal_.transactionState = (uint8_t)AddressTransactionState::Acknowledged;
      markBootJournalDirty();
      flushBootJournal(true);
    }
  }

  void verifyAddressTransaction(uint16_t confirmedId) {
    if (bootJournal_.transactionState == (uint8_t)AddressTransactionState::None) {
      return;
    }
    bootJournal_.transactionConfirmedId = confirmedId;
    bootJournal_.transactionState = (uint8_t)AddressTransactionState::Verified;
    bootJournal_.rollbackNeeded = 0;
    bootJournal_.lastWorkingGatewayId = confirmedId;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation), "address transaction verified");
    markBootJournalDirty();
    flushBootJournal(true);
  }

  void failAddressTransaction() {
    if (bootJournal_.transactionState == (uint8_t)AddressTransactionState::None ||
        bootJournal_.transactionState == (uint8_t)AddressTransactionState::Verified) {
      return;
    }
    bootJournal_.transactionState = (uint8_t)AddressTransactionState::Failed;
    bootJournal_.rollbackNeeded = 1;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation), "address verification failed");
    markBootJournalDirty();
    flushBootJournal(true);
  }

  void recordMutationRequested(uint16_t typeCode, uint8_t subcommand,
                               uint16_t requestedGatewayId, const char* label) {
    if (activeMutationIndex_ >= 0) {
      return;
    }
    const uint8_t index = bootJournal_.mutationHead % TIGO_BOOT_MUTATION_JOURNAL_ENTRIES;
    BootMutationEntry& entry = bootJournal_.mutations[index];
    memset(&entry, 0, sizeof(entry));
    entry.epoch = ntpTimeValid() ? (uint32_t)time(nullptr) : 0;
    entry.typeCode = typeCode;
    entry.subcommand = subcommand;
    entry.result = 0;
    entry.beforeGatewayId = gatewayId_;
    entry.requestedGatewayId = requestedGatewayId;
    entry.beforeNetworkConfirmed = lastNetworkConfirmedNodes_;
    entry.beforeNodeTableHash = currentNodeTableHash();
    bootJournal_.mutationHead = (uint8_t)((index + 1U) % TIGO_BOOT_MUTATION_JOURNAL_ENTRIES);
    if (bootJournal_.mutationCount < TIGO_BOOT_MUTATION_JOURNAL_ENTRIES) {
      ++bootJournal_.mutationCount;
    }
    activeMutationIndex_ = (int8_t)index;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
               label ? label : "state change requested");
    markBootJournalDirty();
  }

  void finishMutationJournal(bool ok) {
    if (activeMutationIndex_ < 0 ||
        activeMutationIndex_ >= (int8_t)TIGO_BOOT_MUTATION_JOURNAL_ENTRIES) {
      return;
    }
    BootMutationEntry& entry = bootJournal_.mutations[(uint8_t)activeMutationIndex_];
    entry.result = ok ? 1 : 2;
    entry.confirmedGatewayId = gatewayId_;
    entry.afterNetworkConfirmed = lastNetworkConfirmedNodes_;
    entry.afterNodeTableHash = currentNodeTableHash();
    activeMutationIndex_ = -1;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation),
               ok ? "state change acknowledged" : "state change failed");
    markBootJournalDirty();
  }

  void loadStateFromPersistentStore() {
    if (!persistentStoreReady_) {
      addEvent("state: preferences unavailable");
      resetPersistentStateToDefaults();
      return;
    }

    const PersistentStateLoadStatus status = persistentStateLoadOrResetControllerState(
        platformRuntime_.persistentStore(),
        TIGO_PERSISTENT_STATE_STORAGE_KEY,
        TIGO_GATEWAY_ID,
        TIGO_INITIAL_PACKET_NUMBER,
        panelFieldCountClamped(),
        &gatewayId_,
        &nextPacketNumber_,
        gatewayLongAddr_,
        sizeof(gatewayLongAddr_),
        &panelFieldCount_,
        panelMap_,
        TIGO_MAX_OPTIMIZERS);
    if (status == PersistentStateLoadStatus::Missing) {
      addEvent("state: no stored preferences state");
      return;
    }
    if (status != PersistentStateLoadStatus::Loaded) {
      addEvent("state: invalid preferences state");
      return;
    }

    if (gatewayId_ != TIGO_GATEWAY_ID) {
      addEvent("state keeps last verified gateway=%04X; configured candidate=%04X tap=%s",
               gatewayId_, TIGO_GATEWAY_ID, gatewayLongAddr_);
    }

    memset(nodeMap_, 0, sizeof(nodeMap_));
    addEvent("state loaded: gateway=%04X packet=%04X panel_fields=%u", gatewayId_, nextPacketNumber_, panelFieldCount_);
  }

  bool saveStateToPersistentStore() {
    if (!persistentStoreReady_) {
      return false;
    }
    return persistentStateSaveToStore(
        platformRuntime_.persistentStore(),
        TIGO_PERSISTENT_STATE_STORAGE_KEY,
        gatewayId_,
        persistedPacketCursor_,
        gatewayLongAddr_,
        panelFieldCount_,
        panelMap_,
        TIGO_MAX_OPTIMIZERS);
  }

  void resetPersistentStateToDefaults() {
    persistentStateResetControllerState(TIGO_GATEWAY_ID,
                                        TIGO_INITIAL_PACKET_NUMBER,
                                        panelFieldCountClamped(),
                                        &gatewayId_,
                                        &nextPacketNumber_,
                                        gatewayLongAddr_,
                                        sizeof(gatewayLongAddr_),
                                        &panelFieldCount_);
  }

  void resetMqttSettingsToDefaults() {
    memset(&mqttSettings_, 0, sizeof(mqttSettings_));
    mqttSettings_.magic = TIGO_MQTT_SETTINGS_MAGIC;
    mqttSettings_.version = TIGO_MQTT_SETTINGS_VERSION;
    mqttSettings_.port = TIGO_MQTT_PORT;
    copyString(mqttSettings_.host, sizeof(mqttSettings_.host), TIGO_MQTT_HOST);
    copyString(mqttSettings_.baseTopic, sizeof(mqttSettings_.baseTopic), TIGO_MQTT_BASE_TOPIC);
    copyString(mqttSettings_.username, sizeof(mqttSettings_.username), TIGO_MQTT_USERNAME);
    copyString(mqttSettings_.password, sizeof(mqttSettings_.password), TIGO_MQTT_PASSWORD);
  }

  void loadPollingSettingsFromPersistentStore() {
    if (!TIGO_RS485_ACTIVE_POLLING) {
      activePollingEnabled_ = false;
      addEvent("polling setting forced disabled by firmware");
      return;
    }
    if (!persistentStoreReady_) {
      return;
    }
    PollingRuntimeSettings stored;
    const size_t read = platformRuntime_.persistentStore().loadBytes(
        TIGO_POLLING_SETTINGS_STORAGE_KEY, &stored, sizeof(stored));
    if (read == 0) {
      return;
    }
    if (read != sizeof(stored) ||
        stored.magic != TIGO_POLLING_SETTINGS_MAGIC ||
        stored.version != TIGO_POLLING_SETTINGS_VERSION ||
        stored.enabled > 1) {
      addEvent("polling setting ignored: invalid preferences state");
      return;
    }
    activePollingEnabled_ = stored.enabled != 0;
    addEvent("polling setting loaded: %s", activePollingEnabled_ ? "enabled" : "disabled");
  }

  bool savePollingSettingsToPersistentStore() {
    if (!persistentStoreReady_) {
      return false;
    }
    PollingRuntimeSettings stored{};
    stored.magic = TIGO_POLLING_SETTINGS_MAGIC;
    stored.version = TIGO_POLLING_SETTINGS_VERSION;
    stored.enabled = activePollingEnabled_ ? 1 : 0;
    return platformRuntime_.persistentStore().saveBytes(
        TIGO_POLLING_SETTINGS_STORAGE_KEY, &stored, sizeof(stored));
  }

  void resetRadioIdentitySettings() {
    workingRadioDescriptorValid_ = false;
    rollbackRadioDescriptorValid_ = false;
    radioJoinSeedValid_ = false;
    memset(workingRadioDescriptor_, 0, sizeof(workingRadioDescriptor_));
    memset(rollbackRadioDescriptor_, 0, sizeof(rollbackRadioDescriptor_));
    memset(radioJoinSeed_, 0, sizeof(radioJoinSeed_));
    workingRadioDescriptorTapLongAddr_[0] = '\0';
    rollbackRadioDescriptorTapLongAddr_[0] = '\0';
    radioJoinSeedProfileFingerprint_ = 0;
  }

  uint32_t radioIdentityChecksum(const RadioIdentitySettings& settings) const {
    RadioIdentitySettings copy = settings;
    copy.checksum = 0;
    return fnv1a32(reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
  }

  bool saveRadioIdentityToPersistentStore() {
    if (!persistentStoreReady_) {
      return false;
    }
    RadioIdentitySettings stored{};
    stored.magic = TIGO_RADIO_IDENTITY_MAGIC;
    stored.version = TIGO_RADIO_IDENTITY_VERSION;
    if (workingRadioDescriptorValid_) {
      stored.flags |= TIGO_RADIO_IDENTITY_HAS_WORKING_DESCRIPTOR;
      memcpy(stored.workingDescriptor, workingRadioDescriptor_, sizeof(stored.workingDescriptor));
      copyString(stored.workingTapLongAddr, sizeof(stored.workingTapLongAddr),
                 workingRadioDescriptorTapLongAddr_);
    }
    if (rollbackRadioDescriptorValid_) {
      stored.flags |= TIGO_RADIO_IDENTITY_HAS_ROLLBACK_DESCRIPTOR;
      memcpy(stored.rollbackDescriptor, rollbackRadioDescriptor_, sizeof(stored.rollbackDescriptor));
      copyString(stored.rollbackTapLongAddr, sizeof(stored.rollbackTapLongAddr),
                 rollbackRadioDescriptorTapLongAddr_);
    }
    if (radioJoinSeedValid_) {
      stored.flags |= TIGO_RADIO_IDENTITY_HAS_JOIN_SEED;
      memcpy(stored.joinSeed, radioJoinSeed_, sizeof(stored.joinSeed));
      stored.joinSeedProfileFingerprint = radioJoinSeedProfileFingerprint_;
    }
    stored.checksum = radioIdentityChecksum(stored);
    return platformRuntime_.persistentStore().saveBytes(
        TIGO_RADIO_IDENTITY_STORAGE_KEY, &stored, sizeof(stored));
  }

  void loadRadioIdentityFromPersistentStore() {
    resetRadioIdentitySettings();
    if (!persistentStoreReady_) {
      return;
    }
    RadioIdentitySettings stored{};
    const size_t read = platformRuntime_.persistentStore().loadBytes(
        TIGO_RADIO_IDENTITY_STORAGE_KEY, &stored, sizeof(stored));
    if (read == 0) {
      addEvent("radio identity: no saved profile");
      return;
    }
    const uint8_t knownFlags = TIGO_RADIO_IDENTITY_HAS_WORKING_DESCRIPTOR |
                               TIGO_RADIO_IDENTITY_HAS_JOIN_SEED |
                               TIGO_RADIO_IDENTITY_HAS_ROLLBACK_DESCRIPTOR;
    if (read != sizeof(stored) ||
        stored.magic != TIGO_RADIO_IDENTITY_MAGIC ||
        stored.version != TIGO_RADIO_IDENTITY_VERSION ||
        (stored.flags & ~knownFlags) != 0 ||
        stored.checksum != radioIdentityChecksum(stored) ||
        stored.workingTapLongAddr[sizeof(stored.workingTapLongAddr) - 1] != '\0' ||
        stored.rollbackTapLongAddr[sizeof(stored.rollbackTapLongAddr) - 1] != '\0') {
      addEvent("radio identity: invalid saved profile ignored");
      return;
    }
    workingRadioDescriptorValid_ =
        (stored.flags & TIGO_RADIO_IDENTITY_HAS_WORKING_DESCRIPTOR) != 0;
    rollbackRadioDescriptorValid_ =
        (stored.flags & TIGO_RADIO_IDENTITY_HAS_ROLLBACK_DESCRIPTOR) != 0;
    radioJoinSeedValid_ = (stored.flags & TIGO_RADIO_IDENTITY_HAS_JOIN_SEED) != 0;
    memcpy(workingRadioDescriptor_, stored.workingDescriptor, sizeof(workingRadioDescriptor_));
    memcpy(rollbackRadioDescriptor_, stored.rollbackDescriptor, sizeof(rollbackRadioDescriptor_));
    memcpy(radioJoinSeed_, stored.joinSeed, sizeof(radioJoinSeed_));
    radioJoinSeedProfileFingerprint_ = stored.joinSeedProfileFingerprint;
    copyString(workingRadioDescriptorTapLongAddr_, sizeof(workingRadioDescriptorTapLongAddr_),
               stored.workingTapLongAddr);
    copyString(rollbackRadioDescriptorTapLongAddr_, sizeof(rollbackRadioDescriptorTapLongAddr_),
               stored.rollbackTapLongAddr);
    addEvent("radio identity loaded: working=%u rollback=%u join_seed=%u source_tap=%s",
             workingRadioDescriptorValid_ ? 1U : 0U,
             rollbackRadioDescriptorValid_ ? 1U : 0U,
             radioJoinSeedValid_ ? 1U : 0U,
             workingRadioDescriptorTapLongAddr_[0] ? workingRadioDescriptorTapLongAddr_ : "-");
  }

  bool radioDescriptorLooksValid(const uint8_t* descriptor) const {
    if (descriptor == nullptr) {
      return false;
    }
    const uint16_t channel = ((uint16_t)descriptor[0] << 8) | descriptor[1];
    const uint16_t panId = ((uint16_t)descriptor[2] << 8) | descriptor[3];
    return channel >= 11 && channel <= 26 && panId != 0 && panId != 0xFFFFU;
  }

  bool radioProfileMismatch() const {
    return currentRadioDescriptorValid_ && workingRadioDescriptorValid_ &&
           memcmp(currentRadioDescriptor_, workingRadioDescriptor_,
                  TIGO_RADIO_DESCRIPTOR_LEN) != 0;
  }

  bool radioJoinSeedMatchesCurrentProfile() const {
    return radioJoinSeedValid_ && currentRadioDescriptorValid_ &&
           radioJoinSeedProfileFingerprint_ ==
               fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN);
  }

  void radioDescriptorFingerprint(const uint8_t* descriptor, char* out, size_t outLen) const {
    if (out == nullptr || outLen == 0) {
      return;
    }
    if (descriptor == nullptr) {
      out[0] = '\0';
      return;
    }
    snprintf(out, outLen, "%08lX",
             (unsigned long)fnv1a32(descriptor, TIGO_RADIO_DESCRIPTOR_LEN));
    out[outLen - 1] = '\0';
  }

  void storeCurrentRadioDescriptor(const uint8_t* descriptor, size_t len, const char* source) {
    if (len < TIGO_RADIO_DESCRIPTOR_LEN || !radioDescriptorLooksValid(descriptor)) {
      return;
    }
    const bool changed = !currentRadioDescriptorValid_ ||
                         memcmp(currentRadioDescriptor_, descriptor, TIGO_RADIO_DESCRIPTOR_LEN) != 0;
    memcpy(currentRadioDescriptor_, descriptor, TIGO_RADIO_DESCRIPTOR_LEN);
    currentRadioDescriptorValid_ = true;
    currentRadioDescriptorUpdatedMs_ = platformMillis();
    copyString(currentRadioDescriptorTapLongAddr_, sizeof(currentRadioDescriptorTapLongAddr_), gatewayLongAddr_);
    bootJournal_.radioDescriptorFingerprint =
        fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN);
    copyString(bootJournal_.tapLongAddr, sizeof(bootJournal_.tapLongAddr), gatewayLongAddr_);
    markBootJournalDirty();
    statusDirty_ = true;
    if (radioJoinSeedValid_ && radioJoinSeedProfileFingerprint_ == 0) {
      radioJoinSeedProfileFingerprint_ = fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN);
      saveRadioIdentityToPersistentStore();
    }
    if (changed) {
      char fingerprint[12];
      radioDescriptorFingerprint(descriptor, fingerprint, sizeof(fingerprint));
      const uint16_t channel = ((uint16_t)descriptor[0] << 8) | descriptor[1];
      const uint16_t panId = ((uint16_t)descriptor[2] << 8) | descriptor[3];
      addEvent("radio profile observed from %s: channel=%u pan=%04X fingerprint=%s tap=%s",
               source ? source : "bus", channel, panId, fingerprint,
               currentRadioDescriptorTapLongAddr_[0] ? currentRadioDescriptorTapLongAddr_ : "-");
    }
  }

  void storeJoinSeed(const uint8_t* body, size_t len) {
    if (body == nullptr || len != TIGO_JOIN_SEED_LEN ||
        body[0] != 0x00 || body[1] != 0x01 ||
        body[2] != 0x00 || body[3] != 0x00 || body[4] != 0x00 || body[5] != 0x01 ||
        body[6] != 0x00 || body[7] != 0xAA) {
      return;
    }
    const uint32_t profileFingerprint = currentRadioDescriptorValid_
        ? fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) : 0;
    const bool changed = !radioJoinSeedValid_ ||
                         memcmp(radioJoinSeed_, body, len) != 0 ||
                         radioJoinSeedProfileFingerprint_ != profileFingerprint;
    memcpy(radioJoinSeed_, body, len);
    radioJoinSeedValid_ = true;
    radioJoinSeedProfileFingerprint_ = profileFingerprint;
    statusDirty_ = true;
    if (changed) {
      if (!saveRadioIdentityToPersistentStore()) {
        addEvent("join seed observed but persistence failed");
      } else {
        addEvent("generic 0x41 join seed observed and saved");
      }
    }
  }

  void markCurrentRadioDescriptorWorking() {
    if (!currentRadioDescriptorValid_ || gatewayLongAddr_[0] == '\0' ||
        strcmp(currentRadioDescriptorTapLongAddr_, gatewayLongAddr_) != 0) {
      return;
    }
    const bool changed = !workingRadioDescriptorValid_ ||
                         memcmp(workingRadioDescriptor_, currentRadioDescriptor_,
                                TIGO_RADIO_DESCRIPTOR_LEN) != 0 ||
                         strcmp(workingRadioDescriptorTapLongAddr_, gatewayLongAddr_) != 0;
    if (!changed) {
      return;
    }
    memcpy(workingRadioDescriptor_, currentRadioDescriptor_, sizeof(workingRadioDescriptor_));
    workingRadioDescriptorValid_ = true;
    copyString(workingRadioDescriptorTapLongAddr_, sizeof(workingRadioDescriptorTapLongAddr_),
               gatewayLongAddr_);
    if (!saveRadioIdentityToPersistentStore()) {
      addEvent("working radio profile proven by power but persistence failed");
      return;
    }
    char fingerprint[12];
    radioDescriptorFingerprint(workingRadioDescriptor_, fingerprint, sizeof(fingerprint));
    addEvent("working radio profile saved after power telemetry: fingerprint=%s tap=%s",
             fingerprint, workingRadioDescriptorTapLongAddr_);
  }

  bool mqttSettingsValid(const MqttRuntimeSettings& settings) const {
    return settings.magic == TIGO_MQTT_SETTINGS_MAGIC &&
           settings.version == TIGO_MQTT_SETTINGS_VERSION &&
           settings.port > 0 &&
           settings.host[sizeof(settings.host) - 1] == '\0' &&
           settings.baseTopic[sizeof(settings.baseTopic) - 1] == '\0' &&
           settings.username[sizeof(settings.username) - 1] == '\0' &&
           settings.password[sizeof(settings.password) - 1] == '\0';
  }

  void loadMqttSettingsFromPersistentStore() {
    if (!persistentStoreReady_) {
      addEvent("mqtt settings: preferences unavailable; using defaults");
      return;
    }
    MqttRuntimeSettings stored;
    const size_t read = platformRuntime_.persistentStore().loadBytes(
        TIGO_MQTT_SETTINGS_STORAGE_KEY, &stored, sizeof(stored));
    if (read != sizeof(stored)) {
      addEvent("mqtt settings: no stored config; using defaults");
      return;
    }
    if (!mqttSettingsValid(stored)) {
      addEvent("mqtt settings: invalid stored config; using defaults");
      return;
    }
    mqttSettings_ = stored;
    addEvent("mqtt settings loaded: %s:%u topic=%s",
             mqttSettings_.host,
             (unsigned)mqttSettings_.port,
             mqttSettings_.baseTopic);
  }

  bool saveMqttSettingsToPersistentStore() {
    if (!persistentStoreReady_) {
      return false;
    }
    return platformRuntime_.persistentStore().saveBytes(
        TIGO_MQTT_SETTINGS_STORAGE_KEY, &mqttSettings_, sizeof(mqttSettings_));
  }

  bool mqttTopicMigrationComplete(uint8_t flag) const {
    return (mqttMigrationFlags_ & flag) == flag;
  }

  void loadMqttTopicMigrationSettingsFromPersistentStore() {
    mqttMigrationFlags_ = 0;
    if (!persistentStoreReady_) {
      return;
    }
    MqttTopicMigrationSettings stored{};
    const size_t read = platformRuntime_.persistentStore().loadBytes(
        TIGO_MQTT_MIGRATION_STORAGE_KEY, &stored, sizeof(stored));
    if (read == sizeof(stored) && stored.magic == TIGO_MQTT_MIGRATION_MAGIC &&
        stored.version == TIGO_MQTT_MIGRATION_VERSION) {
      mqttMigrationFlags_ = stored.completedFlags;
    }
  }

  void markMqttTopicMigrationComplete(uint8_t flag) {
    if (mqttTopicMigrationComplete(flag)) {
      return;
    }
    mqttMigrationFlags_ |= flag;
    if (!persistentStoreReady_) {
      return;
    }
    const MqttTopicMigrationSettings stored{
        TIGO_MQTT_MIGRATION_MAGIC,
        TIGO_MQTT_MIGRATION_VERSION,
        mqttMigrationFlags_,
        0,
    };
    if (!platformRuntime_.persistentStore().saveBytes(
            TIGO_MQTT_MIGRATION_STORAGE_KEY, &stored, sizeof(stored))) {
      addEvent("mqtt topic migration state save failed");
    }
  }

  bool copyValidatedArg(const char* argName, char* dest, size_t destLen, bool required, const char* label) {
    if (!webServer_.hasArg(argName)) {
      if (required) {
        String body = F("{\"ok\":false,\"error\":\"missing_field\",\"field\":\"");
        body += argName;
        body += F("\"}");
        sendJson(400, body);
        return false;
      }
      return true;
    }
    String value = webServer_.arg(argName);
    value.trim();
    if (required && value.length() == 0) {
      String body = F("{\"ok\":false,\"error\":\"empty_field\",\"field\":\"");
      body += argName;
      body += F("\"}");
      sendJson(400, body);
      return false;
    }
    if (value.length() >= destLen) {
      String body = F("{\"ok\":false,\"error\":\"field_too_long\",\"field\":\"");
      body += argName;
      body += F("\",\"message\":\"");
      body += label;
      body += F(" is too long\"}");
      sendJson(400, body);
      return false;
    }
    copyString(dest, destLen, value.c_str());
    return true;
  }

  bool topicLooksValid(const char* topic) const {
    if (topic == nullptr || topic[0] == '\0' || topic[0] == '/' || topic[strlen(topic) - 1] == '/') {
      return false;
    }
    for (const char* p = topic; *p; ++p) {
      if (*p == '#' || *p == '+') {
        return false;
      }
    }
    return true;
  }


  // --------------------------
  // MQTT
  // --------------------------
  void setupMqtt() {
    otxBuildMqttClientId(platformRuntime_, TIGO_HOSTNAME, mqttClientId_, sizeof(mqttClientId_));
    mqttClient_.setServer(mqttSettings_.host, mqttSettings_.port);
    mqttClient_.setBufferSize(TIGO_MQTT_PACKET_SIZE);
    mqttClient_.setSocketTimeout(TIGO_MQTT_SOCKET_TIMEOUT_S);
    mqttClient_.setCallback(handleTigoMqttCallback);
  }

  void subscribeMqttCommands() {
    otxSubscribeMqttCommand(mqttClient_, mqttSettings_.baseTopic, "command/polling/set");
  }

  void maintainMqtt() {
    const OtxMqttMaintainResult result = otxMaintainMqttConnection(
        mqttClient_,
        wifiConnected_,
        mqttSettings_.host,
        mqttClientId_,
        mqttSettings_.username,
        mqttSettings_.password,
        TIGO_MQTT_RECONNECT_EVERY_MS,
        platformMillis(),
        &lastMqttConnectAttemptMs_);
    if (result == OtxMqttMaintainResult::NotReady ||
        result == OtxMqttMaintainResult::WaitingRetry ||
        result == OtxMqttMaintainResult::ConnectFailed) {
      mqttConnected_ = false;
      return;
    }
    mqttConnected_ = true;
    if (result == OtxMqttMaintainResult::ConnectedNow) {
      subscribeMqttCommands();
      addEvent("mqtt connected: %s:%u", mqttSettings_.host, (unsigned)mqttSettings_.port);
      invalidateLegacyDiscovery();
      legacyStateTopicsCleared_ = false;
      invalidNodeStatusTopicsCleared_ = mqttTopicMigrationComplete(TIGO_MQTT_MIGRATION_INVALID_NODE_STATUS);
      deprecatedPanelTelemetryTopicsCleared_ = mqttTopicMigrationComplete(TIGO_MQTT_MIGRATION_PANEL_TELEMETRY);
      deprecatedSystemStatusTopicsCleared_ = mqttTopicMigrationComplete(TIGO_MQTT_MIGRATION_SYSTEM_STATUS);
      if (!invalidNodeStatusTopicsCleared_) {
        invalidNodeStatusClearIndex_ = 0;
        lastInvalidNodeStatusClearMs_ = 0;
      }
      if (!deprecatedPanelTelemetryTopicsCleared_) {
        deprecatedPanelTelemetryClearIndex_ = 0;
        lastDeprecatedPanelTelemetryClearMs_ = 0;
      }
      if (!deprecatedSystemStatusTopicsCleared_) {
        deprecatedSystemStatusClearIndex_ = 0;
        lastDeprecatedSystemStatusClearMs_ = 0;
      }
      statusDirty_ = true;
      legacyStateDirty_ = true;
    }
  }

  void handleMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    char value[24];
    bool enabled = activePollingEnabled_;
    const OtxMqttBoolCommandResult result = otxParseMqttBoolCommand(
        mqttSettings_.baseTopic,
        "command/polling/set",
        topic,
        payload,
        length,
        activePollingEnabled_,
        true,
        &enabled,
        value,
        sizeof(value));
    if (result == OtxMqttBoolCommandResult::NoMatch) {
      return;
    }
    if (result == OtxMqttBoolCommandResult::InvalidPayload) {
      addEvent("ignored mqtt polling cmd: %s", value);
      return;
    }
    setActivePollingEnabled(enabled, "mqtt");
  }

  void makeMqttTopic(const char* suffix, char* out, size_t outLen) const {
    otxMqttMakeTopic(mqttSettings_.baseTopic, suffix, out, outLen);
  }

  void makeChildTopic(const char* prefix, const char* key, char* out, size_t outLen) const {
    otxMqttMakeChildTopic(prefix, key, out, outLen);
  }

  void makeLegacyNodeDeviceId(uint16_t nodeId, char* out, size_t outLen) const {
    otxMakeLegacyNodeDeviceId(nodeId, out, outLen);
  }

  void makeLegacyLabelPrefix(const char* label, char* out, size_t outLen) const {
    otxMakeLegacyLabelPrefix(TIGO_MQTT_LEGACY_BASE_TOPIC, label, out, outLen);
  }

  void makeLegacyDisplayPrefix(const char* label, char* out, size_t outLen) const {
    otxMakeLegacyDisplayPrefix(TIGO_MQTT_LEGACY_BASE_TOPIC, label, out, outLen);
  }

  void makeLegacyNodePrefix(uint16_t nodeId, char* out, size_t outLen) const {
    otxMakeLegacyNodePrefix(TIGO_MQTT_LEGACY_BASE_TOPIC, nodeId, out, outLen);
  }

  bool mqttPublish(const char* suffix, const String& payload, bool retained) {
    return otxMqttPublish(mqttClient_, mqttSettings_.baseTopic, suffix, payload, retained);
  }

  bool mqttPublish(const char* suffix, const char* payload, bool retained) {
    return otxMqttPublish(mqttClient_, mqttSettings_.baseTopic, suffix, payload, retained);
  }

  bool mqttPublishTopic(const char* topic, const String& payload, bool retained) {
    return otxMqttPublishTopic(mqttClient_, topic, payload, retained);
  }

  bool mqttPublishTopic(const char* topic, const char* payload, bool retained) {
    return otxMqttPublishTopic(mqttClient_, topic, payload, retained);
  }

  bool mqttClearRetainedTopic(const char* topic) {
    return otxMqttClearRetainedTopic(mqttClient_, topic);
  }

  bool mqttPublishScalar(const char* suffix, const String& payload, bool retained) {
    return mqttPublish(suffix, payload, retained);
  }

  bool mqttPublishScalar(const char* suffix, const char* payload, bool retained) {
    return mqttPublish(suffix, payload, retained);
  }

  void mqttPublishSubScalar(const char* prefix, const char* key, const char* payload, bool retained) {
    otxMqttPublishSubScalar(mqttClient_, mqttSettings_.baseTopic, prefix, key, payload, retained);
  }

  void mqttPublishSubScalarU32(const char* prefix, const char* key, uint32_t value, bool retained) {
    otxMqttPublishSubScalarU32(mqttClient_, mqttSettings_.baseTopic, prefix, key, value, retained);
  }

  void mqttPublishSubScalarU16(const char* prefix, const char* key, uint16_t value, bool retained) {
    otxMqttPublishSubScalarU16(mqttClient_, mqttSettings_.baseTopic, prefix, key, value, retained);
  }

  void mqttPublishSubScalarFloat(const char* prefix, const char* key, float value, uint8_t decimals, bool retained) {
    otxMqttPublishSubScalarFloat(mqttClient_, mqttSettings_.baseTopic, prefix, key, value, decimals, retained);
  }

  void mqttPublishScalarU32(const char* suffix, uint32_t value, bool retained) {
    otxMqttPublishScalarU32(mqttClient_, mqttSettings_.baseTopic, suffix, value, retained);
  }

  void mqttPublishScalarU16(const char* suffix, uint16_t value, bool retained) {
    otxMqttPublishScalarU16(mqttClient_, mqttSettings_.baseTopic, suffix, value, retained);
  }

  void mqttPublishScalarFloat(const char* suffix, float value, uint8_t decimals, bool retained) {
    otxMqttPublishScalarFloat(mqttClient_, mqttSettings_.baseTopic, suffix, value, decimals, retained);
  }

  void mqttPublishScalarHex4(const char* suffix, uint16_t value, bool retained) {
    otxMqttPublishScalarHex4(mqttClient_, mqttSettings_.baseTopic, suffix, value, retained);
  }

  void makeTelemetryPrimarySuffix(const PowerReport& slot, char* out, size_t outLen) const {
    otxMakeTelemetryPrimarySuffix(slot, out, outLen);
  }

  void makeDiscoveryDeviceName(const PowerReport& slot, char* out, size_t outLen) const {
    otxMakeDiscoveryDeviceName(slot, out, outLen);
  }

  void makeDiscoveryObjectBase(const PowerReport& slot, char* out, size_t outLen) const {
    otxMakeDiscoveryObjectBase(slot, out, outLen);
  }

  String makePanelDisplayName(const char* label) const {
    return otxMakePanelDisplayNameString(label);
  }

  String makeDiscoveryDeviceName(const PowerReport& slot) const {
    return otxMakeDiscoveryDeviceNameString(slot);
  }

  String makeDiscoveryObjectBase(const PowerReport& slot) const {
    return otxMakeDiscoveryObjectBaseString(slot);
  }

  String makeLegacyNodeDeviceId(uint16_t nodeId) const {
    return otxMakeLegacyNodeDeviceIdString(nodeId);
  }

  String makeLegacyLabelPrefix(const char* label) const {
    return otxMakeLegacyLabelPrefixString(TIGO_MQTT_LEGACY_BASE_TOPIC, label);
  }

  String makeLegacyDisplayPrefix(const char* label) const {
    return otxMakeLegacyDisplayPrefixString(TIGO_MQTT_LEGACY_BASE_TOPIC, label);
  }

  String makeLegacyNodePrefix(uint16_t nodeId) const {
    return otxMakeLegacyNodePrefixString(TIGO_MQTT_LEGACY_BASE_TOPIC, nodeId);
  }

  void mqttPublishSubScalar(const String& prefix, const char* key, const String& payload, bool retained) {
    otxMqttPublishSubScalar(mqttClient_, mqttSettings_.baseTopic, prefix, key, payload, retained);
  }

  void mqttPublishSubScalar(const String& prefix, const char* key, const char* payload, bool retained) {
    otxMqttPublishSubScalar(mqttClient_, mqttSettings_.baseTopic, prefix, key, payload, retained);
  }

  String makeHomeAssistantSensorConfigTopic(const String& objectBase, const char* key) const {
    return otxMakeHomeAssistantSensorConfigTopic(TIGO_MQTT_LEGACY_DISCOVERY_PREFIX, objectBase, key);
  }

  void clearHomeAssistantSensorConfigTopic(const String& objectBase, const char* key) {
    mqttClearRetainedTopic(makeHomeAssistantSensorConfigTopic(objectBase, key).c_str());
  }

  void clearRetainedTopicIfConnected(const char* topic) {
    if (!mqttClient_.connected() || topic == nullptr || topic[0] == '\0') {
      return;
    }
    mqttClearRetainedTopic(topic);
  }

  void clearDeprecatedPowerStateTopic(const String& prefix) {
    if (!mqttClient_.connected() || prefix.length() == 0) {
      return;
    }
    mqttClearRetainedTopic((prefix + "/power_out_est_w").c_str());
  }

  void clearDeprecatedPowerStatusTopics() {
    char topic[MQTT_TOPIC_LEN];
    makeMqttTopic("status/power/live_sum_output_w", topic, sizeof(topic));
    clearRetainedTopicIfConnected(topic);
    makeMqttTopic("status/power/held_sum_output_w", topic, sizeof(topic));
    clearRetainedTopicIfConnected(topic);
  }

  void clearDeprecatedSummaryDiscoveryTopics() {
    const String objectBase = "opentaptox_tigo_summary";
    clearHomeAssistantSensorConfigTopic(objectBase, "power_out");
    clearHomeAssistantSensorConfigTopic(objectBase, "power_in");
  }

  void clearHomeAssistantOptimizerDiscoveryByObjectBase(const String& objectBase) {
    for (size_t i = 0; i < (sizeof(TIGO_LEGACY_DISCOVERY_FIELDS) / sizeof(TIGO_LEGACY_DISCOVERY_FIELDS[0])); ++i) {
      clearHomeAssistantSensorConfigTopic(objectBase, TIGO_LEGACY_DISCOVERY_FIELDS[i].key);
    }
    clearHomeAssistantSensorConfigTopic(objectBase, "power_in");
    clearHomeAssistantSensorConfigTopic(objectBase, "long_addr");
    clearHomeAssistantSensorConfigTopic(objectBase, "barcode");
    clearHomeAssistantSensorConfigTopic(objectBase, "panel_label");
    clearHomeAssistantSensorConfigTopic(objectBase, "node_id");
    clearHomeAssistantSensorConfigTopic(objectBase, "short_addr");
    clearHomeAssistantSensorConfigTopic(objectBase, "short_addr_hex");
  }

  void clearHomeAssistantOptimizerDiscoveryByLabel(const char* label) {
    if (label == nullptr || label[0] == '\0') {
      return;
    }
    String labelLower = String(label);
    labelLower.toLowerCase();
    mqttClearRetainedTopic(makeLegacyDeviceConfigTopic(label).c_str());
    clearHomeAssistantOptimizerDiscoveryByObjectBase(String("opentaptox_") + slugifyIdentifier(makePanelDisplayName(label)));
  }

  void clearHomeAssistantOptimizerDiscoveryByNode(uint16_t nodeId) {
    mqttClearRetainedTopic(makeLegacyNodeConfigTopic(nodeId).c_str());
    clearHomeAssistantOptimizerDiscoveryByObjectBase(String("opentaptox_tigo_node_") + String(nodeId));
  }

  void clearLegacyTelemetryPrefix(const String& prefix) {
    if (!mqttClient_.connected() || prefix.length() == 0) {
      return;
    }
    static const char* keys[] = {
      "node_id",
      "short_addr",
      "long_addr",
      "panel_label",
      "unknown_hex",
      "vin_v",
      "vout_v",
      "iin_a",
      "temp_c",
      "power",
      "power_in_w",
      "rssi",
      "duty_pct",
      "label",
    };
    for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); ++i) {
      mqttClearRetainedTopic((prefix + "/" + keys[i]).c_str());
    }
  }

  void clearLegacyLabelArtifacts(const char* label) {
    if (label == nullptr || label[0] == '\0') {
      return;
    }
    clearHomeAssistantOptimizerDiscoveryByLabel(label);
    clearLegacyStateTopic(String(label));
    clearLegacyTelemetryPrefix(makeLegacyLabelPrefix(label));
    clearLegacyTelemetryPrefix(makeLegacyDisplayPrefix(label));
    clearDeprecatedPowerStateTopic(makeLegacyLabelPrefix(label));
    clearDeprecatedPowerStateTopic(makeLegacyDisplayPrefix(label));
  }

  void publishRawFrame(const GatewayFrame& frame) {
    if (!TIGO_MQTT_ENABLE_RAW_FRAME_TOPIC) {
      return;
    }
    queueRawFrameCapture(frame.gatewayId, frame.addrRaw, frame.typeCode,
                         frame.fromGateway, frame.crcOk, lastFrameMs_,
                         frame.payload, frame.payloadLen);
  }

  bool rs485ReplyPending() const {
    return awaitingReceiveResponse_ ||
           (tapCommandActive() && commandState_.phase == TapCommandPhase::WaitingFrame);
  }

  void queueRawFrameCapture(uint16_t gatewayId, uint16_t addrRaw,
                            uint16_t typeCode, bool fromGateway, bool crcOk,
                            uint32_t deviceMs, const uint8_t* payload,
                            size_t payloadLen) {
    if (!TIGO_MQTT_ENABLE_RAW_FRAME_TOPIC) {
      return;
    }
    if (payloadLen > MAX_FRAME_PAYLOAD ||
        (payloadLen > 0 && payload == nullptr)) {
      ++rawFrameCaptureDropped_;
      return;
    }
    if (rawFrameCaptureCount_ >= TIGO_RAW_FRAME_CAPTURE_QUEUE_LEN) {
      ++rawFrameCaptureDropped_;
      return;
    }
    const size_t index =
        (rawFrameCaptureHead_ + rawFrameCaptureCount_) % TIGO_RAW_FRAME_CAPTURE_QUEUE_LEN;
    RawFrameCaptureItem& item = rawFrameCaptureQueue_[index];
    item.gatewayId = gatewayId;
    item.addrRaw = addrRaw;
    item.typeCode = typeCode;
    item.payloadLen = (uint16_t)payloadLen;
    item.deviceMs = deviceMs;
    item.fromGateway = fromGateway;
    item.crcOk = crcOk;
    if (payloadLen > 0) {
      memcpy(item.payload, payload, payloadLen);
    }
    ++rawFrameCaptureCount_;
  }

  void flushPendingRawFrameCapture() {
    if (rawFrameCaptureCount_ == 0 || rs485ReplyPending() ||
        !mqttClient_.connected()) {
      return;
    }
    const RawFrameCaptureItem& item = rawFrameCaptureQueue_[rawFrameCaptureHead_];
    if (!otxPublishRawFrameFields(mqttClient_, mqttSettings_.baseTopic,
                                  item.gatewayId, item.addrRaw, item.typeCode,
                                  item.fromGateway, item.crcOk, item.deviceMs,
                                  item.payload, item.payloadLen)) {
      return;
    }
    rawFrameCaptureHead_ =
        (rawFrameCaptureHead_ + 1U) % TIGO_RAW_FRAME_CAPTURE_QUEUE_LEN;
    --rawFrameCaptureCount_;
  }

  void publishMqttPeriodic(uint32_t now) {
    if (nodeWakeActive_) {
      return;
    }
    // Run at most one one-shot broker migration at a time. This keeps the
    // normal TAP loop responsive while an older retained topic tree is pruned.
    if (!invalidNodeStatusTopicsCleared_) {
      clearInvalidNodeStatusTopics(now);
    } else if (!deprecatedPanelTelemetryTopicsCleared_) {
      clearDeprecatedPanelTelemetryTopics(now);
    } else if (!deprecatedSystemStatusTopicsCleared_) {
      clearDeprecatedSystemStatusTopics(now);
    }
    otxRunMqttPeriodicCycle(
        mqttClient_.connected(),
        now,
        TIGO_MQTT_HEARTBEAT_EVERY_MS,
        TIGO_MQTT_STATUS_EVERY_MS,
        TIGO_MQTT_TELEMETRY_EVERY_MS,
        &lastMqttHeartbeatPublishMs_,
        &lastMqttStatusPublishMs_,
        &lastMqttTelemetryPublishMs_,
        &statusDirty_,
        [this]() { dedupePowerSlots(); },
        [this]() { clearLegacyStateTopics(); },
        [this](uint32_t stepNow) { publishLegacyDiscovery(stepNow); },
        [this](uint32_t stepNow) { publishMqttHeartbeat(stepNow); },
        [this]() { publishMqttStatus(); },
        [this]() { publishAllTelemetry(); });
  }

  void publishMqttHeartbeat(uint32_t now) {
    otxPublishMqttHeartbeat(
        [this](const char* suffix, const char* payload, bool retained) {
          mqttPublishScalar(suffix, payload, retained);
        },
        [this](const char* suffix, uint32_t value, bool retained) {
          mqttPublishScalarU32(suffix, value, retained);
        },
        now);
  }

  void publishMqttStatus() {
    const uint32_t now = platformMillis();
    const AggregatePowerStatus agg = otxBuildAggregatePowerStatus(
        powerSlots_, MAX_POWER_SLOTS, now, TIGO_SAMPLE_FRESH_MS, TIGO_SAMPLE_HOLD_MS);
    char ip[20];
    fillCurrentIp(ip, sizeof(ip));
    const char* wifiMode = apMode_ ? "ap" : (wifiConnected_ ? "sta" : "none");
    const OtxMqttStatusSnapshot snapshot{
        TIGO_PROJECT_TITLE,
        TIGO_HOSTNAME,
        ip,
        wifiMode,
        gatewayLongAddr_,
        lastResetReason_,
        TIGO_FIRMWARE_VERSION,
        webVersionText_,
        wifiConnected_,
        apMode_,
        mqttClient_.connected(),
        activePollingEnabled_,
        gatewayId_,
        nextPacketNumber_,
        lastRequestedPacketNumber_,
        countConfirmedNodeMap(),
        countValidPower(),
        panelFieldCount_,
        TIGO_MAX_OPTIMIZERS,
        framesRx_,
        framesCrcError_,
        pollsSent_,
        pollTimeouts_,
        TIGO_RS485_POLL_INTERVAL_MS,
        TIGO_RS485_POLL_TIMEOUT_MS,
        now,
        ESP.getFreeHeap(),
        TIGO_SAMPLE_FRESH_MS,
        TIGO_SAMPLE_HOLD_MS,
        agg,
    };
    otxPublishMqttStatusSnapshot(
        snapshot,
        [this](const char* suffix, const char* payload, bool retained) {
          mqttPublishScalar(suffix, payload, retained);
        },
        [this](const char* suffix, uint32_t value, bool retained) {
          mqttPublishScalarU32(suffix, value, retained);
        },
        [this](const char* suffix, uint16_t value, bool retained) {
          mqttPublishScalarU16(suffix, value, retained);
        },
        [this](const char* suffix, float value, uint8_t decimals, bool retained) {
          mqttPublishScalarFloat(suffix, value, decimals, retained);
        },
        [this](const char* suffix, uint16_t value, bool retained) {
          mqttPublishScalarHex4(suffix, value, retained);
        },
        [this]() { publishPanelMapMqttStatus(); },
        [this]() { publishNodeMapMqttStatus(); });
  }

  void publishPanelMapMqttStatus() {
    otxPublishPanelMapStatusRows(
        panelMap_,
        panelFieldCount_,
        [this](const char* prefix, const char* key, const char* payload, bool retained) {
          mqttPublishSubScalar(prefix, key, payload, retained);
        },
        [this](const char* prefix, const char* key, uint16_t value, bool retained) {
          mqttPublishSubScalarU16(prefix, key, value, retained);
        },
        [this]() { platformYield(); });
  }

  void publishNodeMapMqttStatus() {
    otxPublishNodeMapStatusRows(
        nodeMap_,
        MAX_NODE_MAP,
        platformMillis(),
        TIGO_SAMPLE_FRESH_MS,
        [this](const char* longAddr) { return lookupPanelLabel(longAddr); },
        [this](uint16_t nodeId) { return findPowerSlotByNodeId(nodeId); },
        [this](const char* prefix, const char* key, const char* payload, bool retained) {
          mqttPublishSubScalar(prefix, key, payload, retained);
        },
        [this](const char* prefix, const char* key, uint16_t value, bool retained) {
          mqttPublishSubScalarU16(prefix, key, value, retained);
        },
        [this](const char* prefix, const char* key, uint32_t value, bool retained) {
          mqttPublishSubScalarU32(prefix, key, value, retained);
        });
  }

  void publishAllTelemetry() {
    otxPublishAllTelemetry(
        powerSlots_,
        MAX_POWER_SLOTS,
        [this](const PowerReport& slot) { publishTelemetry(slot); },
        [this]() { platformYield(); });
  }

  void publishTelemetry(const PowerReport& slot) {
    const uint32_t now = platformMillis();
    const OtxTelemetryPublishValues values = otxBuildTelemetryPublishValues(
        slot, now, TIGO_SAMPLE_FRESH_MS);

    char primarySuffix[MQTT_TOPIC_LEN];
    makeTelemetryPrimarySuffix(slot, primarySuffix, sizeof(primarySuffix));
    otxPublishTelemetryValues(
        primarySuffix,
        slot,
        values,
        [this](const char* prefix, const char* key, const char* payload, bool retained) {
          mqttPublishSubScalar(prefix, key, payload, retained);
        },
        [this](const char* prefix, const char* key, uint32_t value, bool retained) {
          mqttPublishSubScalarU32(prefix, key, value, retained);
        },
        [this](const char* prefix, const char* key, float value, uint8_t decimals, bool retained) {
          mqttPublishSubScalarFloat(prefix, key, value, decimals, retained);
        });

  }

  const char* lookupConfiguredPanelLongAddr(const char* label) const {
    if (label == nullptr || label[0] == '\0') {
      return nullptr;
    }
    for (uint16_t i = 0; i < panelFieldCount_; ++i) {
      if (strcmp(panelMap_[i].label, label) == 0) {
        return panelMap_[i].longAddr;
      }
    }
    return nullptr;
  }

  String makeLegacyDeviceConfigTopic(const char* label) const {
    return otxMakeLegacyDeviceConfigTopic(TIGO_MQTT_LEGACY_DISCOVERY_PREFIX, label);
  }

  String makeLegacyNodeConfigTopic(uint16_t nodeId) const {
    return otxMakeLegacyNodeConfigTopic(TIGO_MQTT_LEGACY_DISCOVERY_PREFIX, nodeId);
  }

  bool clearLegacyDeviceDiscovery(const char* label) {
    clearHomeAssistantOptimizerDiscoveryByLabel(label);
    return true;
  }

  bool clearLegacyNodeDiscovery(uint16_t nodeId) {
    clearHomeAssistantOptimizerDiscoveryByNode(nodeId);
    return true;
  }

  void clearLegacyStateTopic(const String& deviceId) {
    if (!mqttClient_.connected() || deviceId.length() == 0) {
      return;
    }
    String topic = String(TIGO_MQTT_LEGACY_BASE_TOPIC);
    if (!topic.endsWith("/")) {
      topic += "/";
    }
    topic += deviceId;
    topic += "/state";
    mqttClearRetainedTopic(topic.c_str());
  }

  void clearLegacyStateTopics() {
    if (!TIGO_MQTT_ENABLE_LEGACY_STATE_TOPICS || legacyStateTopicsCleared_ || !mqttClient_.connected()) {
      return;
    }

    const uint32_t now = platformMillis();
    if (lastLegacyStateClearMs_ != 0 &&
        elapsed(now, lastLegacyStateClearMs_) < TIGO_MQTT_DISCOVERY_STEP_EVERY_MS) {
      return;
    }
    lastLegacyStateClearMs_ = now;

    if (legacyDiscoveryClearIndex_ == 0) {
      ++legacyDiscoveryClearIndex_;
      clearLegacyStateTopic("summary");
      clearDeprecatedPowerStatusTopics();
      clearDeprecatedPowerStateTopic(String(TIGO_MQTT_LEGACY_BASE_TOPIC) + "/summary");
      platformYield();
      return;
    }

    if (legacyDiscoveryClearIndex_ <= TIGO_MAX_OPTIMIZERS) {
      const uint16_t i = (uint16_t)(legacyDiscoveryClearIndex_ - 1U);
      ++legacyDiscoveryClearIndex_;
      clearLegacyStateTopic(String(panelMap_[i].label));
      clearLegacyTelemetryPrefix(makeLegacyLabelPrefix(panelMap_[i].label));
      clearLegacyTelemetryPrefix(makeLegacyDisplayPrefix(panelMap_[i].label));
      clearDeprecatedPowerStateTopic(makeLegacyLabelPrefix(panelMap_[i].label));
      clearDeprecatedPowerStateTopic(makeLegacyDisplayPrefix(panelMap_[i].label));
      platformYield();
      return;
    }

    uint16_t slotIndex = (uint16_t)(legacyDiscoveryClearIndex_ - 1U - TIGO_MAX_OPTIMIZERS);
    while (slotIndex < MAX_POWER_SLOTS && !powerSlots_[slotIndex].valid) {
      ++legacyDiscoveryClearIndex_;
      ++slotIndex;
    }
    if (slotIndex < MAX_POWER_SLOTS) {
      ++legacyDiscoveryClearIndex_;
      const String deviceId = makeLegacyNodeDeviceId(powerSlots_[slotIndex].nodeId);
      clearLegacyStateTopic(deviceId);
      clearDeprecatedPowerStateTopic(makeLegacyNodePrefix(powerSlots_[slotIndex].nodeId));
      if (powerSlots_[slotIndex].panelLabel[0]) {
        clearDeprecatedPowerStateTopic(makeLegacyLabelPrefix(powerSlots_[slotIndex].panelLabel));
        clearDeprecatedPowerStateTopic(makeLegacyDisplayPrefix(powerSlots_[slotIndex].panelLabel));
      }
      platformYield();
      return;
    }

    legacyStateTopicsCleared_ = true;
  }

  void clearInvalidNodeStatusTopics(uint32_t now) {
    if (!TIGO_MQTT_CLEAN_INVALID_NODE_STATUS_TOPICS ||
        invalidNodeStatusTopicsCleared_ ||
        !mqttClient_.connected()) {
      return;
    }
    if (lastInvalidNodeStatusClearMs_ != 0 &&
        elapsed(now, lastInvalidNodeStatusClearMs_) < TIGO_MQTT_DISCOVERY_STEP_EVERY_MS) {
      return;
    }
    lastInvalidNodeStatusClearMs_ = now;

    const uint16_t nodeId = (uint16_t)(TIGO_MQTT_INVALID_NODE_STATUS_BASE + invalidNodeStatusClearIndex_);
    char prefix[MQTT_TOPIC_LEN];
    snprintf(prefix, sizeof(prefix), "status/nodes/node_%u", (unsigned)nodeId);
    prefix[sizeof(prefix) - 1] = '\0';
    static const char* keys[] = {
      "node_id",
      "long_addr",
      "panel_label",
      "has_power",
      "short_addr_hex",
      "age_ms",
      "fresh",
    };
    for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); ++i) {
      char suffix[MQTT_TOPIC_LEN];
      char topic[MQTT_TOPIC_LEN];
      makeChildTopic(prefix, keys[i], suffix, sizeof(suffix));
      makeMqttTopic(suffix, topic, sizeof(topic));
      mqttClearRetainedTopic(topic);
    }

    ++invalidNodeStatusClearIndex_;
    if (invalidNodeStatusClearIndex_ >= TIGO_MQTT_INVALID_NODE_STATUS_COUNT) {
      invalidNodeStatusTopicsCleared_ = true;
      markMqttTopicMigrationComplete(TIGO_MQTT_MIGRATION_INVALID_NODE_STATUS);
      addEvent("mqtt retained invalid node status cleanup complete");
    }
    platformYield();
  }

  void clearDeprecatedPanelTelemetryTopics(uint32_t now) {
    if (deprecatedPanelTelemetryTopicsCleared_ || !mqttClient_.connected()) {
      return;
    }
    if (lastDeprecatedPanelTelemetryClearMs_ != 0 &&
        elapsed(now, lastDeprecatedPanelTelemetryClearMs_) < TIGO_MQTT_DISCOVERY_STEP_EVERY_MS) {
      return;
    }
    lastDeprecatedPanelTelemetryClearMs_ = now;

    const uint16_t fieldCount = panelFieldCountClamped();
    if (deprecatedPanelTelemetryClearIndex_ >= fieldCount) {
      deprecatedPanelTelemetryTopicsCleared_ = true;
      markMqttTopicMigrationComplete(TIGO_MQTT_MIGRATION_PANEL_TELEMETRY);
      addEvent("mqtt deprecated panel telemetry cleanup complete");
      return;
    }

    const UserPanelMapEntry& panel = panelMap_[deprecatedPanelTelemetryClearIndex_++];
    if (panel.label[0] == '\0') {
      return;
    }
    char prefix[MQTT_TOPIC_LEN];
    snprintf(prefix, sizeof(prefix), "telemetry/%s", panel.label);
    prefix[sizeof(prefix) - 1] = '\0';
    static const char* keys[] = {
      "panel_label",
      "node_id",
      "short_addr_hex",
      "long_addr",
      "ip",
      "gateway_id_hex",
      "hold_weight",
      "live_power_in_w",
      "held_power_in_w",
    };
    char topic[MQTT_TOPIC_LEN];
    makeMqttTopic(prefix, topic, sizeof(topic));
    mqttClearRetainedTopic(topic);
    for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); ++i) {
      char suffix[MQTT_TOPIC_LEN];
      makeChildTopic(prefix, keys[i], suffix, sizeof(suffix));
      makeMqttTopic(suffix, topic, sizeof(topic));
      mqttClearRetainedTopic(topic);
    }
    platformYield();
  }

  void clearDeprecatedSystemStatusTopics(uint32_t now) {
    if (deprecatedSystemStatusTopicsCleared_ || !mqttClient_.connected()) {
      return;
    }
    if (lastDeprecatedSystemStatusClearMs_ != 0 &&
        elapsed(now, lastDeprecatedSystemStatusClearMs_) < TIGO_MQTT_DISCOVERY_STEP_EVERY_MS) {
      return;
    }
    lastDeprecatedSystemStatusClearMs_ = now;

    static const char* keys[] = {
      "hostname", "ip", "wifi_mode", "wifi_connected", "ap_mode", "mqtt_connected",
      "gateway_id_hex", "gateway_long_addr", "next_packet_hex", "last_requested_packet_hex",
      "frames_rx", "frames_crc_error", "polls_sent", "poll_timeouts", "polling_enabled",
      "poll_interval_ms", "poll_timeout_ms", "node_count", "power_count", "uptime_ms",
      "free_heap", "reset_reason", "firmware_version", "panel_field_count", "max_optimizers",
      "version_text",
    };
    if (deprecatedSystemStatusClearIndex_ >= (sizeof(keys) / sizeof(keys[0]))) {
      deprecatedSystemStatusTopicsCleared_ = true;
      markMqttTopicMigrationComplete(TIGO_MQTT_MIGRATION_SYSTEM_STATUS);
      addEvent("mqtt deprecated system status cleanup complete");
      return;
    }

    char suffix[MQTT_TOPIC_LEN];
    char topic[MQTT_TOPIC_LEN];
    makeChildTopic("status/system", keys[deprecatedSystemStatusClearIndex_++], suffix, sizeof(suffix));
    makeMqttTopic(suffix, topic, sizeof(topic));
    mqttClearRetainedTopic(topic);
    platformYield();
  }

  void publishLegacyTelemetryPrefix(const String& prefix,
                                    const PowerReport& slot) {
    mqttPublishTopic((prefix + "/node_id").c_str(), String(slot.nodeId), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/short_addr").c_str(), hex4(slot.shortAddr), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/long_addr").c_str(), slot.longAddr, TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/panel_label").c_str(), slot.panelLabel, TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/unknown_hex").c_str(), slot.unknownHex, TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/vin_v").c_str(), String(slot.vinV, 3), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/vout_v").c_str(), String(slot.voutV, 3), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/iin_a").c_str(), String(slot.iinA, 3), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/temp_c").c_str(), String(slot.tempC, 1), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/power").c_str(), String(slot.powerInW, 3), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/rssi").c_str(), String(slot.rssi), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/duty_pct").c_str(), String(slot.dutyPct, 2), TIGO_MQTT_RETAIN_LEGACY_STATE);
    mqttPublishTopic((prefix + "/label").c_str(), slot.panelLabel, TIGO_MQTT_RETAIN_LEGACY_STATE);
  }

  void publishLegacyTelemetry(const PowerReport& slot) {
    if (!TIGO_MQTT_ENABLE_LEGACY_STATE_TOPICS || !mqttClient_.connected()) {
      return;
    }

    const String deviceId = makeLegacyNodeDeviceId(slot.nodeId);
    clearLegacyStateTopic(deviceId);
    const String nodePrefix = makeLegacyNodePrefix(slot.nodeId);
    clearDeprecatedPowerStateTopic(nodePrefix);
    publishLegacyTelemetryPrefix(nodePrefix, slot);
    if (slot.panelLabel[0]) {
      const String labelPrefix = makeLegacyLabelPrefix(slot.panelLabel);
      const String displayPrefix = makeLegacyDisplayPrefix(slot.panelLabel);
      clearDeprecatedPowerStateTopic(labelPrefix);
      clearDeprecatedPowerStateTopic(displayPrefix);
      publishLegacyTelemetryPrefix(labelPrefix, slot);
      publishLegacyTelemetryPrefix(displayPrefix, slot);
    }
  }

  void publishLegacyDiscovery(uint32_t now) {
    if (!TIGO_MQTT_ENABLE_LEGACY_DISCOVERY || legacyDiscoveryPublished_ || !mqttClient_.connected()) {
      return;
    }
    if (elapsed(now, lastLegacyDiscoveryStepMs_) < TIGO_MQTT_DISCOVERY_STEP_EVERY_MS) {
      return;
    }
    lastLegacyDiscoveryStepMs_ = now;
    while (legacyDiscoveryPublishSlotIndex_ < MAX_POWER_SLOTS) {
      const size_t slotIndex = legacyDiscoveryPublishSlotIndex_++;
      if (!powerSlots_[slotIndex].valid) {
        continue;
      }
      bool alreadyPublished = false;
      for (size_t j = 0; j < slotIndex; ++j) {
        if (powerSlots_[j].valid &&
            powerSlots_[j].nodeId == powerSlots_[slotIndex].nodeId) {
          alreadyPublished = true;
          break;
        }
      }
      if (alreadyPublished) {
        continue;
      }
      if (!publishLegacyNodeDiscovery(powerSlots_[slotIndex])) {
        logLegacyDiscoveryFailure("mqtt discovery publish failed");
      }
      platformYield();
      return;
    }

    if (!publishLegacySummaryDiscovery()) {
      logLegacyDiscoveryFailure("mqtt discovery publish failed");
      return;
    }
    legacyDiscoveryPublished_ = true;
  }

  void invalidateLegacyDiscovery() {
    legacyDiscoveryPublished_ = false;
    legacyDiscoveryClearIndex_ = 0;
    legacyDiscoveryPublishSlotIndex_ = 0;
    lastLegacyDiscoveryStepMs_ = 0;
    lastLegacyStateClearMs_ = 0;
  }

  void logLegacyDiscoveryFailure(const char* message) {
    const uint32_t now = platformMillis();
    if (elapsed(now, lastLegacyDiscoveryFailureLogMs_) < 5000UL) {
      return;
    }
    lastLegacyDiscoveryFailureLogMs_ = now;
    addEvent("%s", message);
  }

  bool publishLegacyNodeDiscovery(const PowerReport& slot) {
    const String deviceName = makeDiscoveryDeviceName(slot);
    const String objectBase = makeDiscoveryObjectBase(slot);
    char telemetrySuffix[MQTT_TOPIC_LEN];
    makeTelemetryPrimarySuffix(slot, telemetrySuffix, sizeof(telemetrySuffix));
    const String statePrefix = String(mqttSettings_.baseTopic) + "/" + telemetrySuffix;

    auto publishNodeSensor = [&](const char* key,
                                 const char* stateSuffix,
                                 const char* unit,
                                 const char* deviceClass,
                                 const char* entityCategory,
                                 bool measurement) -> bool {
      const String configTopic = makeHomeAssistantSensorConfigTopic(objectBase, key);
      const String stateTopic = statePrefix + "/" + stateSuffix;
      const String objectId = objectBase + "_" + key;
      const String sensorName = deviceName + " " + key;
      const String body = otxBuildOptimizerSensorDiscoveryJson(
          sensorName, objectId, stateTopic, objectBase, deviceName,
          unit, deviceClass, entityCategory, measurement,
          TIGO_FIRMWARE_VERSION, slot.longAddr);
      return mqttPublishTopic(configTopic.c_str(), body, true);
    };

    bool ok = true;
    if (slot.panelLabel[0]) {
      clearHomeAssistantOptimizerDiscoveryByNode(slot.nodeId);
    }
    for (size_t i = 0; i < (sizeof(TIGO_LEGACY_DISCOVERY_FIELDS) / sizeof(TIGO_LEGACY_DISCOVERY_FIELDS[0])); ++i) {
      const LegacyDiscoveryField& field = TIGO_LEGACY_DISCOVERY_FIELDS[i];
      const bool measurement = strcmp(field.key, "power") == 0;
      ok = publishNodeSensor(field.key, field.jsonField, field.unit, field.deviceClass, nullptr, measurement) && ok;
      platformYield();
    }
    ok = publishNodeSensor("node_id", "node_id", "", nullptr, "diagnostic", false) && ok;
    platformYield();
    ok = publishNodeSensor("short_addr_hex", "short_addr_hex", "", nullptr, "diagnostic", false) && ok;
    platformYield();
    ok = publishNodeSensor("long_addr", "long_addr", "", nullptr, "diagnostic", false) && ok;
    platformYield();
    ok = publishNodeSensor("panel_label", "panel_label", "", nullptr, "diagnostic", false) && ok;
    platformYield();

    if (!ok) {
      addEvent("mqtt discovery failed: node %u", (unsigned)slot.nodeId);
    }
    return ok;
  }

  bool publishLegacySummaryDiscovery() {
    const String objectBase = "opentaptox_tigo_summary";
    clearDeprecatedSummaryDiscoveryTopics();

    auto publishSummarySensor = [&](const char* key,
                                    const String& stateTopic,
                                    const char* name,
                                    const char* unit,
                                    const char* deviceClass,
                                    bool measurement) -> bool {
      const String configTopic = makeHomeAssistantSensorConfigTopic(objectBase, key);
      const String body = otxBuildSummarySensorDiscoveryJson(
          objectBase, stateTopic, key, name, unit, deviceClass,
          measurement, TIGO_FIRMWARE_VERSION);
      return mqttPublishTopic(configTopic.c_str(), body, true);
    };

    bool ok = true;
    ok = publishSummarySensor("power", String(mqttSettings_.baseTopic) + "/status/power/held_sum_input_w", "Tigo Leistung", "W", "power", true) && ok;
    ok = publishSummarySensor("nodes", String(mqttSettings_.baseTopic) + "/status/plant/power_node_count", "Tigo Nodes", "", "", false) && ok;
    if (!ok) {
      addEvent("mqtt discovery failed: summary");
    }
    return ok;
  }

  void publishLegacySummary() {
    if (!TIGO_MQTT_ENABLE_LEGACY_STATE_TOPICS || !mqttClient_.connected()) {
      return;
    }

    const AggregatePowerStatus agg = otxBuildAggregatePowerStatus(
        powerSlots_, MAX_POWER_SLOTS, platformMillis(), TIGO_SAMPLE_FRESH_MS, TIGO_SAMPLE_HOLD_MS);
    clearLegacyStateTopic("summary");
    char prefix[MQTT_TOPIC_LEN];
    snprintf(prefix, sizeof(prefix), "%s/summary", TIGO_MQTT_LEGACY_BASE_TOPIC);
    prefix[sizeof(prefix) - 1] = '\0';
    clearDeprecatedPowerStateTopic(String(prefix));
    char topic[MQTT_TOPIC_LEN];
    char payload[20];
    // Legacy summary consumers expect a stable plant total. On passive TAP sniffing the
    // per-node reports arrive staggered, so the strict "live" window often collapses to a
    // partial or zero sum even while valid node telemetry is present.
    makeChildTopic(prefix, "power", topic, sizeof(topic));
    formatFloat(agg.heldSumInputW, 3, payload, sizeof(payload));
    mqttPublishTopic(topic, payload, TIGO_MQTT_RETAIN_LEGACY_STATE);
    makeChildTopic(prefix, "nodes", topic, sizeof(topic));
    formatUnsigned16(countValidPower(), payload, sizeof(payload));
    mqttPublishTopic(topic, payload, TIGO_MQTT_RETAIN_LEGACY_STATE);
  }

  // --------------------------
  // WiFi / OTA / Web
  // --------------------------
  void connectWifi() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        addEvent("wifi sta disconnected: reason=%u",
                 (unsigned int)info.wifi_sta_disconnected.reason);
      }
    });

    if (strlen(TIGO_WIFI_SSID) > 0) {
      WiFi.hostname(TIGO_HOSTNAME);
      WiFi.begin(TIGO_WIFI_SSID, TIGO_WIFI_PASSWORD);
      const uint32_t start = platformMillis();
      while (WiFi.status() != WL_CONNECTED && elapsed(platformMillis(), start) < 12000UL) {
        platformDelayMilliseconds(250);
        platformYield();
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected_ = true;
      apMode_ = false;
      lastWifiStatus_ = WL_CONNECTED;
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      char ip[20];
      fillCurrentIp(ip, sizeof(ip));
      addEvent("wifi sta connected: %s rssi=%ld", ip, (long)WiFi.RSSI());
      if (TIGO_ENABLE_MDNS) {
        mdnsStarted_ = MDNS.begin(TIGO_HOSTNAME);
        if (mdnsStarted_) {
          addEvent("mdns started: %s.local", TIGO_HOSTNAME);
        } else {
          addEvent("mdns start failed");
        }
      }
    } else {
      const int networks = WiFi.scanNetworks(false, true);
      bool targetVisible = false;
      int32_t targetRssi = 0;
      int32_t targetChannel = 0;
      int32_t targetAuth = -1;
      for (int i = 0; i < networks; ++i) {
        if (WiFi.SSID(i) == TIGO_WIFI_SSID) {
          targetVisible = true;
          targetRssi = WiFi.RSSI(i);
          targetChannel = WiFi.channel(i);
          targetAuth = (int32_t)WiFi.encryptionType(i);
          break;
        }
      }
      addEvent("wifi sta connect failed: status=%d target_visible=%d rssi=%ld channel=%ld auth=%ld",
               (int)WiFi.status(), targetVisible ? 1 : 0,
               (long)targetRssi, (long)targetChannel, (long)targetAuth);
      WiFi.scanDelete();
      WiFi.mode(WIFI_AP);
      if (strlen(TIGO_AP_PASSWORD) > 0) {
        WiFi.softAP(TIGO_AP_SSID, TIGO_AP_PASSWORD);
      } else {
        WiFi.softAP(TIGO_AP_SSID);
      }
      wifiConnected_ = false;
      apMode_ = true;
      lastWifiStatus_ = WiFi.status();
      mdnsStarted_ = false;
      char ip[20];
      fillCurrentIp(ip, sizeof(ip));
      addEvent("wifi ap started: %s", ip);
    }
  }

  void printSerialWifiStatus(uint32_t now) {
    if (TIGO_SERIAL_WIFI_STATUS_EVERY_MS == 0 ||
        (lastSerialWifiStatusMs_ != 0 &&
         elapsed(now, lastSerialWifiStatusMs_) < TIGO_SERIAL_WIFI_STATUS_EVERY_MS)) {
      return;
    }
    lastSerialWifiStatusMs_ = now;
    char ip[20];
    fillCurrentIp(ip, sizeof(ip));
    const wl_status_t status = WiFi.status();
    const bool staConnected = status == WL_CONNECTED;
    const char* mode = apMode_ ? "ap" : (staConnected ? "sta" : "none");
    Serial.printf("OTX wifi t=%lu mode=%s status=%d ip=%s rssi=%ld ssid=%s bssid=%s heap=%lu\r\n",
                  (unsigned long)now,
                  mode,
                  (int)status,
                  ip,
                  staConnected ? (long)WiFi.RSSI() : 0L,
                  staConnected ? WiFi.SSID().c_str() : "-",
                  staConnected ? WiFi.BSSIDstr().c_str() : "-",
                  (unsigned long)ESP.getFreeHeap());
  }

  void setupOta() {
    if (TIGO_ENABLE_OTA) {
      ArduinoOTA.setHostname(TIGO_HOSTNAME);
      ArduinoOTA.onStart([this]() {
        otaInProgress_ = true;
        pollingEnabledBeforeOta_ = activePollingEnabled_;
        awaitingReceiveResponse_ = false;
        activePollingEnabled_ = false;
        abortTapCommand("ota start");
        nodeWakeActive_ = false;
        bootCcaStep_ = UINT8_MAX;
        bootCcaWaitingStep_ = UINT8_MAX;
        if (mqttClient_.connected()) {
          mqttClient_.disconnect();
        }
        addEvent("ota start");
      });
      ArduinoOTA.onEnd([this]() {
        otaInProgress_ = false;
        addEvent("ota end");
      });
      ArduinoOTA.onError([this](ota_error_t error) {
        otaInProgress_ = false;
        addEvent("ota error=%u", (unsigned int)error);
        setActivePollingEnabled(pollingEnabledBeforeOta_, "ota error recovery");
      });
      ArduinoOTA.begin();
    }
  }

  void setupWeb() {
    webServer_.on("/", [this]() { handleRoot(); });
    webServer_.on("/app.js", [this]() { handleAppJs(); });
    webServer_.on("/logo.svg", [this]() { handleLogoSvg(); });
    webServer_.on("/api/status", [this]() { handleApiStatus(); });
    webServer_.on("/api/live-frame", [this]() { handleApiLiveFrame(); });
    webServer_.on("/api/interesting-frames", [this]() { handleApiInterestingFrames(); });
    webServer_.on("/api/interesting-frames/clear", [this]() { handleApiInterestingFramesClear(); });
    webServer_.on("/api/power", [this]() { handleApiPower(); });
    webServer_.on("/api/node-map", [this]() { handleApiNodeMap(); });
    webServer_.on("/api/events", [this]() { handleApiEvents(); });
    webServer_.on("/api/polling", [this]() { handleApiPolling(); });
    webServer_.on("/api/polling/set", [this]() { handleApiPollingSet(); });
    webServer_.on("/api/polling/seed", [this]() { handleApiPollingSeed(); });
    webServer_.on("/api/radio-profile", [this]() { handleApiRadioProfile(); });
    webServer_.on("/api/mqtt-settings", [this]() { handleApiMqttSettings(); });
    webServer_.on("/api/mqtt-settings/save", HTTP_POST, [this]() { handleApiMqttSettingsSave(); });
    webServer_.on("/api/reboot", HTTP_POST, [this]() { handleApiReboot(); });
    webServer_.on("/api/command/warm-reboot", HTTP_POST,
                  [this]() { handleApiWarmReboot(); });
    webServer_.on("/api/command/receive-bootstrap", HTTP_POST,
                  [this]() { handleApiReceiveBootstrap(); });
    webServer_.on("/api/command/replay-session", HTTP_POST,
                  [this]() { handleApiReplaySession(); });
    webServer_.on("/api/trace-replay", [this]() { handleApiTraceReplayStatus(); });
    webServer_.on("/api/trace-replay/result", [this]() { handleApiTraceReplayResult(); });
    webServer_.on("/api/command/trace-replay-plan", HTTP_POST,
                  [this]() { handleApiTraceReplayPlan(); });
    webServer_.on("/api/boot-journal", [this]() { handleApiBootJournal(); });
    webServer_.on("/api/panel-map", [this]() { handleApiPanelMap(); });
    webServer_.on("/api/panel-map/save", HTTP_POST, [this]() { handleApiPanelMapSave(); });
    webServer_.on("/api/command/ping", [this]() { handleApiPing(); });
    webServer_.on("/api/command/rsd-control", HTTP_POST,
                  [this]() { handleApiRsdControl(); });
    webServer_.on("/api/command/version", [this]() { handleApiVersion(); });
    webServer_.on("/api/command/node-table", [this]() { handleApiNodeTable(); });
    webServer_.on("/api/command/network-status", [this]() { handleApiNetworkStatus(); });
    webServer_.on("/api/command/radio-config", [this]() { handleApiRadioConfig(); });
    webServer_.on("/api/command/radio-profile/save-current", HTTP_POST,
                  [this]() { handleApiRadioProfileSaveCurrent(); });
    webServer_.on("/api/command/radio-profile/apply-working", HTTP_POST,
                  [this]() { handleApiRadioProfileApplyWorking(); });
    webServer_.on("/api/command/radio-profile/apply-rollback", HTTP_POST,
                  [this]() { handleApiRadioProfileApplyRollback(); });
    webServer_.on("/api/command/enumerate", HTTP_POST,
                  [this]() { handleApiEnumerate(); });
    webServer_.on("/api/command/simple-frame", [this]() { handleApiSimpleFrame(); });
    webServer_.on("/api/command/rewrite-pv-config", [this]() { handleApiRewritePvConfig(); });
    webServer_.on("/api/command/force-learn", [this]() { handleApiForceLearn(); });
    webServer_.on("/api/command/continue-wake", [this]() { handleApiContinueWake(); });
    webServer_.on("/api/command/hold-learn", HTTP_POST,
                  [this]() { handleApiHoldLearn(); });
    webServer_.on("/api/command/full-cca-replay", HTTP_POST,
                  [this]() { handleApiFullCcaReplay(); });
    webServer_.on("/api/command/rebuild-node-table", HTTP_POST,
                  [this]() { handleApiRebuildNodeTable(); });
    webServer_.on("/api/command/pv-config-set", [this]() { handleApiPvConfigSet(); });
    webServer_.on("/api/command/pv-subcmd", [this]() { handleApiPvSubcmd(); });
    webServer_.on("/api/command/node-text", [this]() { handleApiNodeText(); });
    webServer_.on("/api/command/node-op", [this]() { handleApiNodeOp(); });
    webServer_.onNotFound([this]() { webServer_.send(404, "text/plain", "not found"); });
    webServer_.enableDelay(false);
    webServer_.begin();
    lastWebRequestMs_ = platformMillis();
    lastWebServerRecoverMs_ = lastWebRequestMs_;
    addEvent("web server started: port=80");
  }

  void restartWebServer(const char* reason) {
    webServer_.client().stop();
    webServer_.close();
    platformDelayMilliseconds(20);
    webServer_.begin();
    const uint32_t now = platformMillis();
    lastWebRequestMs_ = now;
    lastWebServerRecoverMs_ = now;
    addEvent("web server restarted: %s", reason ? reason : "unknown");
  }

  void maintainWifiAndWeb() {
    const wl_status_t status = WiFi.status();
    if (status == lastWifiStatus_) {
      return;
    }
    const wl_status_t previous = lastWifiStatus_;
    lastWifiStatus_ = status;
    const bool wasConnected = previous == WL_CONNECTED;
    const bool isConnected = status == WL_CONNECTED;
    wifiConnected_ = isConnected && !apMode_;
    if (!wasConnected && isConnected && !apMode_) {
      if (TIGO_ENABLE_MDNS && !mdnsStarted_) {
        mdnsStarted_ = MDNS.begin(TIGO_HOSTNAME);
      }
      restartWebServer("wifi reconnect");
      char ip[20];
      fillCurrentIp(ip, sizeof(ip));
      addEvent("wifi sta reconnected: %s rssi=%ld", ip, (long)WiFi.RSSI());
    } else if (wasConnected && !isConnected && !apMode_) {
      addEvent("wifi sta disconnected: status=%d", (int)status);
    }
  }

  void markWebRequest() {
    lastWebRequestMs_ = platformMillis();
  }

  void serviceWebServer() {
    if (rs485ReplyPending()) {
      return;
    }
    for (uint8_t i = 0; i < 4; ++i) {
      webServer_.handleClient();
      platformYield();
    }
  }

  void recoverWebServerIfIdle(uint32_t now) {
    if (TIGO_WEB_RECOVER_AFTER_IDLE_MS == 0) {
      return;
    }
    if (elapsed(now, lastWebRequestMs_) < TIGO_WEB_RECOVER_AFTER_IDLE_MS ||
        elapsed(now, lastWebServerRecoverMs_) < TIGO_WEB_RECOVER_EVERY_MS) {
      return;
    }
    restartWebServer("idle watchdog");
  }

  void sendJson(int statusCode, const String& body) {
    markWebRequest();
    webServer_.sendHeader("Access-Control-Allow-Origin", "*");
    webServer_.sendHeader("Cache-Control", "no-store");
    webServer_.send(statusCode, "application/json", body);
  }

  void sendJson(const String& body) {
    sendJson(200, body);
  }

  void beginJsonStream() {
    markWebRequest();
    webServer_.sendHeader("Access-Control-Allow-Origin", "*");
    webServer_.sendHeader("Cache-Control", "no-store");
    webServer_.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer_.send(200, "application/json", "");
  }

  void sendJsonObjectFieldSeparator(bool& firstField) {
    if (!firstField) {
      if (jsonBuildTarget_) {
        *jsonBuildTarget_ += ',';
      } else {
        webServer_.sendContent(",");
      }
    }
    firstField = false;
  }

  void sendJsonObjectFieldString(bool& firstField, const char* key, const char* value) {
    char escaped[384];
    char line[448];
    jsonEscapeToBuffer(value, escaped, sizeof(escaped));
    sendJsonObjectFieldSeparator(firstField);
    snprintf(line, sizeof(line), "\"%s\":\"%s\"", key, escaped);
    line[sizeof(line) - 1] = '\0';
    if (jsonBuildTarget_) {
      *jsonBuildTarget_ += line;
    } else {
      webServer_.sendContent(line);
    }
  }

  void sendJsonObjectFieldRaw(bool& firstField, const char* key, const char* rawValue) {
    char line[96];
    sendJsonObjectFieldSeparator(firstField);
    snprintf(line, sizeof(line), "\"%s\":%s", key, rawValue);
    line[sizeof(line) - 1] = '\0';
    if (jsonBuildTarget_) {
      *jsonBuildTarget_ += line;
    } else {
      webServer_.sendContent(line);
    }
  }

  void handleRoot() {
    markWebRequest();
    webServer_.sendHeader("Cache-Control", "no-store");
    webServer_.send_P(200, "text/html", INDEX_HTML);
  }

  void handleAppJs() {
    markWebRequest();
    webServer_.sendHeader("Cache-Control", "no-store");
    webServer_.send_P(200, "application/javascript", APP_JS);
  }

  void handleLogoSvg() {
    markWebRequest();
    static const char logo[] PROGMEM = R"SVG(<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 128 128'>
<rect width='128' height='128' fill='white'/>
<g fill='#111' stroke='#111' stroke-width='2' stroke-linejoin='round'>
<polygon points='15,27 42,27 49,34 49,45 39,45 39,37 15,37'/>
<polygon points='39,37 62,37 70,45 70,58 58,58 58,48 39,48'/>
<polygon points='58,48 74,48 80,54 80,66 66,66 66,58 58,58'/>
<polygon points='79,55 88,55 92,59 92,66 79,66'/>
<polygon points='90,65 96,69 96,86 90,90 84,86 84,69'/>
<polygon points='94,92 100,98 95,104 88,100'/>
<polygon points='82,92 88,98 83,104 76,100'/>
<polygon points='90,104 96,110 91,116 84,112'/>
<polygon points='62,78 112,78 104,109 54,109' fill='none'/>
<polygon points='60,74 110,74 118,80 112,84 62,84 54,80'/>
<line x1='70' y1='78' x2='62' y2='109'/>
<line x1='82' y1='78' x2='74' y2='109'/>
<line x1='94' y1='78' x2='86' y2='109'/>
<line x1='106' y1='78' x2='98' y2='109'/>
<line x1='58' y1='88' x2='108' y2='88'/>
<line x1='56' y1='98' x2='106' y2='98'/>
</g>
<g fill='#111' font-family='monospace' font-size='12' font-weight='700'>
<text x='95' y='72'>0</text>
<text x='82' y='72'>1</text>
<text x='89' y='84'>0</text>
</g>
</svg>)SVG";
    webServer_.send_P(200, "image/svg+xml", logo);
  }

  void handleApiStatus() {
    const uint32_t now = platformMillis();
    const AggregatePowerStatus agg = otxBuildAggregatePowerStatus(
        powerSlots_, MAX_POWER_SLOTS, now, TIGO_SAMPLE_FRESH_MS, TIGO_SAMPLE_HOLD_MS);
    char tmp[20];
    char frameTs[16];
    char ip[20];
    String body;
    body.reserve(4096);
    jsonBuildTarget_ = &body;
    body += "{";
    bool firstField = true;

    fillCurrentIp(ip, sizeof(ip));
    if (lastFrameValid_) {
      formatFrameTimestamp(now, lastFrameMs_, frameTs, sizeof(frameTs));
    } else {
      copyString(frameTs, sizeof(frameTs), "00:00:00");
    }

    sendJsonObjectFieldString(firstField, "project_title", TIGO_PROJECT_TITLE);
    sendJsonObjectFieldString(firstField, "hostname", TIGO_HOSTNAME);
    sendJsonObjectFieldString(firstField, "wifi_mode", apMode_ ? "ap" : (wifiConnected_ ? "sta" : "none"));
    sendJsonObjectFieldString(firstField, "ip", ip);
    sendJsonObjectFieldRaw(firstField, "mqtt_connected", mqttClient_.connected() ? "true" : "false");
    formatHex4(gatewayId_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "gateway_id_hex", tmp);
    sendJsonObjectFieldString(firstField, "gateway_long_addr", gatewayLongAddr_);
    formatHex4(nextPacketNumber_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "next_packet_hex", tmp);
    formatHex4(lastRequestedPacketNumber_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "last_requested_packet_hex", tmp);
    formatHex4(confirmedPacketCursor_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "confirmed_packet_cursor_hex", tmp);
    formatHex4(persistedPacketCursor_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "persisted_packet_cursor_hex", tmp);
    sendJsonObjectFieldString(firstField, "boot_cursor_strategy", bootCursorStrategyName(bootCursorStrategy_));
    sendJsonObjectFieldRaw(firstField, "cursor_confirmed_this_boot", cursorConfirmedThisBoot_ ? "true" : "false");
    formatUnsigned32(cursorForwardResyncCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "cursor_forward_resync_count", tmp);
    formatUnsigned32(cursorForwardDistance_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "cursor_forward_distance", tmp);
    formatUnsigned32(cursorDuplicateResponseCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "cursor_duplicate_response_count", tmp);
    formatUnsigned32(cursorMalformedPayloadCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "cursor_malformed_payload_count", tmp);
    formatUnsigned32(cursorPersistenceHoldCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "cursor_persistence_hold_count", tmp);
    formatUnsigned32(cursorCheckpointFailureCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "cursor_checkpoint_failure_count", tmp);
    sendJsonObjectFieldRaw(firstField, "cursor_stall_recovery_attempted",
                           cursorStallRecoveryAttempted_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "cursor_stall_recovery_active",
                           targetedCursorRecoveryActive_ ? "true" : "false");
    formatUnsigned32(framesRx_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "frames_rx", tmp);
    formatUnsigned32(framesCrcError_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "frames_crc_error", tmp);
    sendJsonObjectFieldRaw(firstField, "last_frame_valid", lastFrameValid_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "last_frame_crc_ok", lastFrameCrcOk_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "last_frame_from_gateway", lastFrameFromGateway_ ? "true" : "false");
    formatHex4(lastFrameGatewayId_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "last_frame_gateway_id_hex", tmp);
    formatHex4(lastFrameAddrRaw_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "last_frame_addr_raw_hex", tmp);
    formatHex4(lastFrameTypeCode_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "last_frame_type_code_hex", tmp);
    formatUnsigned16(lastFramePayloadLen_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "last_frame_payload_len", tmp);
    formatUnsigned32(lastFrameValid_ ? elapsed(now, lastFrameMs_) : 0UL, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "last_frame_age_ms", tmp);
    sendJsonObjectFieldString(firstField, "last_frame_timestamp", frameTs);
    char liveLine[320];
    buildLastFrameLiveLine(now, liveLine, sizeof(liveLine));
    sendJsonObjectFieldString(firstField, "last_frame_live_line", liveLine);
    sendJsonObjectFieldString(firstField, "last_frame_payload_preview_hex", lastFramePayloadPreviewHex_);
    sendJsonObjectFieldRaw(firstField, "last_frame_payload_truncated", lastFramePayloadTruncated_ ? "true" : "false");
    const bool tapLinkUp = (lastTapResponseMs_ != 0 && elapsed(now, lastTapResponseMs_) <= TIGO_TAP_LINK_FRESH_MS);
    sendJsonObjectFieldRaw(firstField, "tap_link_up", tapLinkUp ? "true" : "false");
    formatUnsigned32(lastTapResponseMs_ != 0 ? elapsed(now, lastTapResponseMs_) : 0UL, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "tap_link_age_ms", tmp);
    formatUnsigned32(tapResponsesRx_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "tap_responses_rx", tmp);
    formatUnsigned32(pollsSent_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "polls_sent", tmp);
    formatUnsigned32(pollTimeouts_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "poll_timeouts", tmp);
    sendJsonObjectFieldRaw(firstField, "polling_enabled", activePollingEnabled_ ? "true" : "false");
    formatUnsigned16(countValidNodeMap(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_count", tmp);
    formatUnsigned16(countConfirmedNodeMap(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_confirmed_count", tmp);
    formatUnsigned16(countPendingNodeMap(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_pending_count", tmp);
    formatUnsigned16(countValidPower(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "power_count", tmp);
    formatUnsigned16(panelFieldCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "panel_field_count", tmp);
    formatUnsigned32(now, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "uptime_ms", tmp);
    formatUnsigned32(ESP.getFreeHeap(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "free_heap", tmp);
    sendJsonObjectFieldString(firstField, "reset_reason", lastResetReason_);
    sendJsonObjectFieldString(firstField, "firmware_version", TIGO_FIRMWARE_VERSION);
    sendJsonObjectFieldString(firstField, "version_text", webVersionText_);
    const TapObservedState tapState = classifyTapState(now);
    sendJsonObjectFieldString(firstField, "tap_state", tapObservedStateName(tapState));
    sendJsonObjectFieldString(firstField, "tap_state_action", tapStateRecommendedAction(tapState));
    sendJsonObjectFieldString(firstField, "boot_path", tapBootPathName(tapBootPath_));
    sendJsonObjectFieldString(firstField, "warm_attach_phase", warmAttachPhaseName(warmAttachPhase_));
    sendJsonObjectFieldRaw(firstField, "read_only_warm_attach", TIGO_READ_ONLY_WARM_ATTACH ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "read_only_state_protected", readOnlyWarmAttachProtectsState_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "recovery_authorized", recoveryAuthorized_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "replay_exclusive_mode", replayExclusiveMode_ ? "true" : "false");
    sendJsonObjectFieldString(firstField, "rsd_control_state",
                              !lastRsdControlKnown_ ? "unknown"
                              : lastRsdRunState_ ? "run" : "stop");
    formatUnsigned32(lastRsdControlKnown_ ? elapsed(now, lastRsdControlAckMs_) : 0UL,
                     tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "rsd_control_age_ms", tmp);
    sendJsonObjectFieldRaw(firstField, "electrical_release_observed", hasFreshReleasedPower(now) ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "electrical_release_stable", hasStableReleasedPower(now) ? "true" : "false");
    formatUnsigned32(releasedPowerEvidenceCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "released_power_evidence_count", tmp);
    formatUnsigned32(firstReceiveResponseMs_ ? elapsed(firstReceiveResponseMs_, bootStartedMs_) : 0, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "first_0149_after_boot_ms", tmp);
    formatUnsigned32(firstPowerFrameMs_ ? elapsed(firstPowerFrameMs_, bootStartedMs_) : 0, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "first_power_after_boot_ms", tmp);
    formatUnsigned32(destructiveManagementFramesTx_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "state_changing_frames_tx", tmp);
    formatUnsigned32(activeRfFramesTx_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "active_rf_frames_tx", tmp);
    formatUnsigned32((uint32_t)rawFrameCaptureCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "raw_frame_capture_queue_depth", tmp);
    formatUnsigned32(rawFrameCaptureDropped_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "raw_frame_capture_dropped", tmp);
    sendJsonObjectFieldString(firstField, "node_table_hash_fnv1a32", hex8(bootJournal_.nodeTableHash).c_str());
    formatFloat(agg.liveSumInputW, 3, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "live_sum_input_w", tmp);
    formatFloat(agg.heldSumInputW, 3, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "held_sum_input_w", tmp);
    formatUnsigned16(agg.freshNodes, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "fresh_nodes", tmp);
    formatUnsigned16(agg.staleNodes, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "stale_nodes", tmp);
    formatUnsigned16(agg.expiredNodes, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "expired_nodes", tmp);
    formatUnsigned32(agg.newestSampleAgeMs, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "newest_sample_age_ms", tmp);
    formatUnsigned32(agg.oldestSampleAgeMs, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "oldest_sample_age_ms", tmp);
    formatUnsigned32(agg.avgSampleAgeMs, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "avg_sample_age_ms", tmp);
    sendJsonObjectFieldRaw(firstField, "command_busy", tapCommandActive() ? "true" : "false");
    sendJsonObjectFieldString(firstField, "command_name", tapCommandActive() ? activeTapCommandName() : lastCommandName_);
    sendJsonObjectFieldString(firstField, "command_state", tapCommandActive() ? activeTapCommandStatus() : lastCommandMessage_);
    formatHex4(lastCommandResponseType_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "last_command_response_type_hex", tmp);
    formatHex4(lastCommandResponseGatewayId_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "last_command_response_gateway_hex", tmp);
    snprintf(tmp, sizeof(tmp), "0x%02X", (unsigned)lastCommandResponseDsn_);
    sendJsonObjectFieldString(firstField, "last_command_response_dsn_hex", tmp);
    sendJsonObjectFieldRaw(firstField, "node_wake_active", nodeWakeActive_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "node_wake_completed", nodeWakeCompleted_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "node_wake_learn_active", nodeWakeLearnActive_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "node_wake_learn_wait_countdown", nodeWakeLearnWaitCountdown_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "node_wake_skip_learn", nodeWakeSkipLearn_ ? "true" : "false");
    formatUnsigned32(nodeWakeStep_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_wake_step", tmp);
    formatUnsigned32(countWakeCandidateNodes(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_wake_candidates", tmp);
    sendJsonObjectFieldRaw(firstField, "node_seed_enabled", TIGO_ENABLE_NODE_SEED ? "true" : "false");
    sendJsonObjectFieldString(firstField, "node_seed_state", nodeSeedStateName());
    formatUnsigned32(nodeSeedPanelCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_seed_panel_count", tmp);
    formatUnsigned32(nodeSeedChunksTotal_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_seed_chunks_total", tmp);
    formatUnsigned32(nodeSeedChunksAcked_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_seed_chunks_acked", tmp);
    formatUnsigned32(nodeSeedNextPanelIndex_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "node_seed_next_panel_index", tmp);
    sendJsonObjectFieldString(firstField, "last_seed_error", lastSeedError_);
    sendJsonObjectFieldString(firstField, "node_table_state",
                              countConfirmedNodeMap() > 0 ? "confirmed" :
                              (countPendingNodeMap() > 0 ? "pending" :
                              (nodeSeedState_ == NodeSeedState::VerifyNodeTable ? "verifying" :
                               (lastNodeTableMs_ != 0 ? "empty" : "unknown"))));
    formatUnsigned32(lastNodeTableStart_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "last_node_table_start", tmp);
    formatUnsigned32(lastNodeTableEntryCount_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "last_node_table_count", tmp);
    sendJsonObjectFieldRaw(firstField, "network_status_valid", lastNetworkStatusValid_ ? "true" : "false");
    formatUnsigned32(lastNetworkStatusMs_ != 0 ? elapsed(now, lastNetworkStatusMs_) : 0UL,
                     tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_status_age_ms", tmp);
    formatUnsigned32(lastNetworkMode_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_mode", tmp);
    formatUnsigned32(lastNetworkCountdown_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_countdown", tmp);
    formatUnsigned32(lastNetworkFlags_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_flags", tmp);
    formatUnsigned32(lastNetworkConfirmedNodes_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_confirmed_nodes", tmp);
    formatUnsigned32(lastNetworkExpectedNodes_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_expected_nodes", tmp);
    // Compatibility aliases used by older status consumers.
    formatUnsigned32(lastNetworkExpectedNodes_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_configured_nodes", tmp);
    formatUnsigned32(lastNetworkConfirmedNodes_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "network_active_nodes", tmp);
    snprintf(tmp, sizeof(tmp), "0x%02X", (unsigned)lastPvSubcommand_);
    tmp[sizeof(tmp) - 1] = '\0';
    sendJsonObjectFieldString(firstField, "last_pv_subcmd_hex", lastPvSubcommand_ != 0 ? tmp : "");
    sendJsonObjectFieldString(firstField, "last_pv_request_hex", lastPvSubcommandRequestHex_);
    formatUnsigned32(lastPvSubcommandRequestLen_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "last_pv_request_len", tmp);
    sendJsonObjectFieldRaw(firstField, "last_pv_request_truncated", lastPvSubcommandRequestTruncated_ ? "true" : "false");
    sendJsonObjectFieldString(firstField, "last_pv_ack_hex", lastPvSubcommandAckHex_);
    formatUnsigned32(lastPvAckStatusFlags_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "last_pv_ack_tx_buffers_free", tmp);
    sendJsonObjectFieldRaw(firstField, "last_pv_ack_status", tmp);
    snprintf(tmp, sizeof(tmp), "0x%02X", (unsigned)lastPvAckStatusFlags_);
    tmp[sizeof(tmp) - 1] = '\0';
    sendJsonObjectFieldString(firstField, "last_pv_ack_status_hex", lastPvAckStatusFlags_ != 0 ? tmp : "");
    snprintf(tmp, sizeof(tmp), "0x%02X", (unsigned)lastPvAckResponseSubcmd_);
    tmp[sizeof(tmp) - 1] = '\0';
    sendJsonObjectFieldString(firstField, "last_pv_ack_rsp_subcmd_hex", lastPvAckResponseSubcmd_ != 0 ? tmp : "");
    sendJsonObjectFieldString(firstField, "last_pv_ack_body_hex", lastPvAckBodyHex_);
    char radioFingerprint[12];
    radioDescriptorFingerprint(currentRadioDescriptorValid_ ? currentRadioDescriptor_ : nullptr,
                               radioFingerprint, sizeof(radioFingerprint));
    sendJsonObjectFieldRaw(firstField, "radio_profile_valid", currentRadioDescriptorValid_ ? "true" : "false");
    formatUnsigned32(currentRadioDescriptorValid_
        ? (((uint16_t)currentRadioDescriptor_[0] << 8) | currentRadioDescriptor_[1]) : 0,
        tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "radio_channel", tmp);
    if (currentRadioDescriptorValid_) {
      formatHex4(((uint16_t)currentRadioDescriptor_[2] << 8) | currentRadioDescriptor_[3], tmp, sizeof(tmp));
    } else {
      tmp[0] = '\0';
    }
    sendJsonObjectFieldString(firstField, "radio_pan_id_hex", tmp);
    sendJsonObjectFieldString(firstField, "radio_profile_fingerprint_fnv1a32", radioFingerprint);
    sendJsonObjectFieldRaw(firstField, "working_radio_profile_saved", workingRadioDescriptorValid_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "rollback_radio_profile_saved", rollbackRadioDescriptorValid_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "radio_join_seed_saved", radioJoinSeedValid_ ? "true" : "false");
    const bool joinSeedMatchesCurrent = radioJoinSeedValid_ && currentRadioDescriptorValid_ &&
        radioJoinSeedProfileFingerprint_ == fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN);
    sendJsonObjectFieldRaw(firstField, "radio_join_seed_matches_current_profile",
                           joinSeedMatchesCurrent ? "true" : "false");
    body += "}";
    jsonBuildTarget_ = nullptr;
    sendJson(body);
  }

  void handleApiLiveFrame() {
    const uint32_t now = platformMillis();
    char tmp[20];
    char liveLine[320];
    char frameTs[16];
    buildLastFrameLiveLine(now, liveLine, sizeof(liveLine));
    if (lastFrameValid_) {
      formatFrameTimestamp(now, lastFrameMs_, frameTs, sizeof(frameTs));
    } else {
      copyString(frameTs, sizeof(frameTs), "00:00:00");
    }

    String body;
    body.reserve(640);
    jsonBuildTarget_ = &body;
    body += "{";
    bool firstField = true;
    sendJsonObjectFieldRaw(firstField, "valid", lastFrameValid_ ? "true" : "false");
    formatUnsigned32(framesRx_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "counter", tmp);
    sendJsonObjectFieldString(firstField, "line", liveLine);
    sendJsonObjectFieldString(firstField, "timestamp", frameTs);
    formatUnsigned32(lastFrameValid_ ? elapsed(now, lastFrameMs_) : 0UL, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "age_ms", tmp);
    formatHex4(lastFrameTypeCode_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "type_hex", tmp);
    sendJsonObjectFieldString(firstField, "type_name", frameTypeName(lastFrameTypeCode_));
    sendJsonObjectFieldRaw(firstField, "crc_ok", lastFrameCrcOk_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "from_gateway", lastFrameFromGateway_ ? "true" : "false");
    formatHex4(lastFrameAddrRaw_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "addr_raw_hex", tmp);
    formatHex4(lastFrameGatewayId_, tmp, sizeof(tmp));
    sendJsonObjectFieldString(firstField, "gateway_id_hex", tmp);
    formatUnsigned16(lastFramePayloadLen_, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "payload_len", tmp);
    sendJsonObjectFieldString(firstField, "payload_preview_hex", lastFramePayloadPreviewHex_);
    sendJsonObjectFieldRaw(firstField, "payload_truncated", lastFramePayloadTruncated_ ? "true" : "false");
    const bool tapLinkUp = (lastTapResponseMs_ != 0 && elapsed(now, lastTapResponseMs_) <= TIGO_TAP_LINK_FRESH_MS);
    sendJsonObjectFieldRaw(firstField, "tap_link_up", tapLinkUp ? "true" : "false");
    formatUnsigned32(lastTapResponseMs_ != 0 ? elapsed(now, lastTapResponseMs_) : 0UL, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "tap_link_age_ms", tmp);
    body += "}";
    jsonBuildTarget_ = nullptr;
    sendJson(body);
  }

  void handleApiPower() {
    const uint32_t now = platformMillis();
    const AggregatePowerStatus agg = otxBuildAggregatePowerStatus(
        powerSlots_, MAX_POWER_SLOTS, now, TIGO_SAMPLE_FRESH_MS, TIGO_SAMPLE_HOLD_MS);
    beginJsonStream();
    char body[192];
    char liveSumBuf[20], heldSumBuf[20], freshBuf[8], staleBuf[8], avgBuf[16];
    formatFloat(agg.liveSumInputW, 3, liveSumBuf, sizeof(liveSumBuf));
    formatFloat(agg.heldSumInputW, 3, heldSumBuf, sizeof(heldSumBuf));
    formatUnsigned16(agg.freshNodes, freshBuf, sizeof(freshBuf));
    formatUnsigned16(agg.staleNodes, staleBuf, sizeof(staleBuf));
    formatUnsigned32(agg.avgSampleAgeMs, avgBuf, sizeof(avgBuf));
    snprintf(body, sizeof(body),
             "{\"summary\":{\"live_sum_input_w\":%s,\"held_sum_input_w\":%s,\"fresh_nodes\":%s,"
             "\"stale_nodes\":%s,\"avg_sample_age_ms\":%s},\"power\":[",
             liveSumBuf, heldSumBuf, freshBuf, staleBuf, avgBuf);
    body[sizeof(body) - 1] = '\0';
    webServer_.sendContent(body);
    bool first = true;
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (!powerSlots_[i].valid) {
        continue;
      }
      const bool addComma = !first;
      first = false;
      const uint32_t ageMs = otxSampleAgeMs(powerSlots_[i], now);
      const bool fresh = otxIsFreshAge(ageMs, TIGO_SAMPLE_FRESH_MS);
      const float holdWeight = otxHeldWeightForAge(ageMs, TIGO_SAMPLE_FRESH_MS, TIGO_SAMPLE_HOLD_MS);
      char item[416];
      char escPanel[24], escLong[40], shortHex[7];
      char nodeIdBuf[8], vinBuf[20], voutBuf[20], iinBuf[20], powerInBuf[20];
      char livePowerBuf[20], heldPowerBuf[20], tempBuf[20], rssiBuf[8], ageBuf[16];
      jsonEscapeToBuffer(powerSlots_[i].panelLabel, escPanel, sizeof(escPanel));
      jsonEscapeToBuffer(powerSlots_[i].longAddr, escLong, sizeof(escLong));
      formatHex4(powerSlots_[i].shortAddr, shortHex, sizeof(shortHex));
      formatUnsigned16(powerSlots_[i].nodeId, nodeIdBuf, sizeof(nodeIdBuf));
      formatFloat(powerSlots_[i].vinV, 3, vinBuf, sizeof(vinBuf));
      formatFloat(powerSlots_[i].voutV, 3, voutBuf, sizeof(voutBuf));
      formatFloat(powerSlots_[i].iinA, 3, iinBuf, sizeof(iinBuf));
      formatFloat(powerSlots_[i].powerInW, 3, powerInBuf, sizeof(powerInBuf));
      formatFloat(fresh ? powerSlots_[i].powerInW : 0.0f, 3, livePowerBuf, sizeof(livePowerBuf));
      formatFloat(powerSlots_[i].powerInW * holdWeight, 3, heldPowerBuf, sizeof(heldPowerBuf));
      formatFloat(powerSlots_[i].tempC, 1, tempBuf, sizeof(tempBuf));
      formatUnsigned32(powerSlots_[i].rssi, rssiBuf, sizeof(rssiBuf));
      formatUnsigned32(ageMs, ageBuf, sizeof(ageBuf));
      snprintf(item, sizeof(item),
               "%s{\"panel_label\":\"%s\",\"node_id\":%s,\"short_addr_hex\":\"%s\",\"long_addr\":\"%s\","
               "\"vin_v\":%s,\"vout_v\":%s,\"iin_a\":%s,\"power\":%s,"
               "\"live_power_in_w\":%s,\"held_power_in_w\":%s,\"temp_c\":%s,\"rssi\":%s,"
               "\"age_ms\":%s,\"fresh\":%s}",
               addComma ? "," : "", escPanel, nodeIdBuf, shortHex, escLong, vinBuf, voutBuf, iinBuf,
               powerInBuf, livePowerBuf, heldPowerBuf, tempBuf, rssiBuf, ageBuf,
               fresh ? "true" : "false");
      item[sizeof(item) - 1] = '\0';
      webServer_.sendContent(item);
      platformYield();
    }
    webServer_.sendContent("]}");
    webServer_.sendContent("");
  }

  void handleApiNodeMap() {
    beginJsonStream();
    webServer_.sendContent("{\"nodes\":[");
    bool first = true;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid) {
        continue;
      }
      const bool addComma = !first;
      first = false;
      char item[320];
      char escLong[40];
      char nodeIdBuf[8];
      char rawNodeIdHex[7];
      jsonEscapeToBuffer(nodeMap_[i].longAddr, escLong, sizeof(escLong));
      formatUnsigned16(nodeMap_[i].nodeId, nodeIdBuf, sizeof(nodeIdBuf));
      formatHex4(nodeMap_[i].rawNodeId, rawNodeIdHex, sizeof(rawNodeIdHex));
      const char* label = lookupPanelLabel(nodeMap_[i].longAddr);
      const PowerReport* power = findPowerSlotByNodeId(nodeMap_[i].nodeId);
      char escLabel[24];
      escLabel[0] = '\0';
      if (label) {
        jsonEscapeToBuffer(label, escLabel, sizeof(escLabel));
      }
      if (power) {
        const uint32_t ageMs = otxSampleAgeMs(*power, platformMillis());
        char ageBuf[16];
        char shortHex[7];
        formatUnsigned32(ageMs, ageBuf, sizeof(ageBuf));
        formatHex4(power->shortAddr, shortHex, sizeof(shortHex));
        snprintf(item, sizeof(item),
                 "%s{\"node_id\":%s,\"raw_node_id_hex\":\"%s\",\"pending\":%s,\"rf_confirmed\":%s,\"long_addr\":\"%s\",\"panel_label\":\"%s\",\"has_power\":true,"
                 "\"short_addr_hex\":\"%s\",\"age_ms\":%s,\"fresh\":%s}",
                 addComma ? "," : "", nodeIdBuf, rawNodeIdHex, nodeMap_[i].pending ? "true" : "false",
                 nodeMap_[i].rfConfirmed ? "true" : "false",
                 escLong, label ? escLabel : "", shortHex, ageBuf,
                 otxIsFreshAge(ageMs, TIGO_SAMPLE_FRESH_MS) ? "true" : "false");
      } else if (label) {
        snprintf(item, sizeof(item),
                 "%s{\"node_id\":%s,\"raw_node_id_hex\":\"%s\",\"pending\":%s,\"rf_confirmed\":%s,\"long_addr\":\"%s\",\"panel_label\":\"%s\",\"has_power\":false}",
                 addComma ? "," : "", nodeIdBuf, rawNodeIdHex, nodeMap_[i].pending ? "true" : "false",
                 nodeMap_[i].rfConfirmed ? "true" : "false",
                 escLong, escLabel);
      } else {
        snprintf(item, sizeof(item),
                 "%s{\"node_id\":%s,\"raw_node_id_hex\":\"%s\",\"pending\":%s,\"rf_confirmed\":%s,\"long_addr\":\"%s\",\"has_power\":false}",
                 addComma ? "," : "", nodeIdBuf, rawNodeIdHex, nodeMap_[i].pending ? "true" : "false",
                 nodeMap_[i].rfConfirmed ? "true" : "false", escLong);
      }
      item[sizeof(item) - 1] = '\0';
      webServer_.sendContent(item);
      platformYield();
    }
    webServer_.sendContent("]}");
    webServer_.sendContent("");
  }


  void handleApiPanelMap() {
    beginJsonStream();
    char body[96];
    snprintf(body, sizeof(body), "{\"panel_field_count\":%u,\"max_optimizers\":%u,\"panel_map\":[",
             (unsigned)panelFieldCount_, (unsigned)TIGO_MAX_OPTIMIZERS);
    body[sizeof(body) - 1] = '\0';
    webServer_.sendContent(body);
    for (uint16_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
      char item[144];
      char escLabel[24], escLong[40];
      jsonEscapeToBuffer(panelMap_[i].label, escLabel, sizeof(escLabel));
      jsonEscapeToBuffer(panelMap_[i].longAddr, escLong, sizeof(escLong));
      snprintf(item, sizeof(item), "%s{\"index\":%u,\"label\":\"%s\",\"long_addr\":\"%s\"}",
               i ? "," : "", (unsigned)(i + 1U), escLabel, escLong);
      item[sizeof(item) - 1] = '\0';
      webServer_.sendContent(item);
      platformYield();
    }
    webServer_.sendContent("]}");
    webServer_.sendContent("");
  }

  void handleApiPanelMapSave() {
    char previousLongAddrs[TIGO_MAX_OPTIMIZERS][17];
    for (uint16_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
      copyString(previousLongAddrs[i], sizeof(previousLongAddrs[i]), panelMap_[i].longAddr);
    }
    const uint16_t previousPanelFieldCount = panelFieldCount_;
    const uint16_t requestedCount = webServer_.hasArg("panel_count")
        ? parseQueryU16("panel_count", panelFieldCount_)
        : panelFieldCount_;
    panelFieldCount_ = clampPanelFieldCountValue(requestedCount);

    for (uint16_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
      if (i >= panelFieldCount_) {
        panelMap_[i].longAddr[0] = '\0';
        continue;
      }
      String argName = String("optimizer_") + String(i + 1U);
      String legacyArgName = panelMap_[i].label;
      bool hasValue = false;
      String v;
      if (webServer_.hasArg(argName)) {
        v = webServer_.arg(argName);
        hasValue = true;
      } else if (webServer_.hasArg(legacyArgName)) {
        v = webServer_.arg(legacyArgName);
        hasValue = true;
      }
      if (!hasValue) {
        continue;
      }
      v.trim();
      v.toUpperCase();
      if (v.length() > 0) {
        uint8_t bin[8];
        if (!hex16ToBytes(v.c_str(), bin)) {
          panelFieldCount_ = previousPanelFieldCount;
          for (uint16_t j = 0; j < TIGO_MAX_OPTIMIZERS; ++j) {
            copyString(panelMap_[j].longAddr, sizeof(panelMap_[j].longAddr), previousLongAddrs[j]);
          }
          String body = F("{\"ok\":false,\"error\":\"invalid long address\",\"field\":\"");
          body += argName;
          body += F("\",\"message\":\"long address must be exactly 16 hexadecimal characters or empty\"}");
          sendJson(400, body);
          return;
        }
      }
      copyString(panelMap_[i].longAddr, sizeof(panelMap_[i].longAddr), v.c_str());
    }
    refreshPanelLabelsFromNodeMap();
    markPersistentStateDirty();
    if (!flushPersistentState(true)) {
      panelFieldCount_ = previousPanelFieldCount;
      for (uint16_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
        copyString(panelMap_[i].longAddr, sizeof(panelMap_[i].longAddr), previousLongAddrs[i]);
      }
      refreshPanelLabelsFromNodeMap();
      sendJson(500, F("{\"ok\":false,\"error\":\"persist_failed\",\"message\":\"optimizer configuration could not be saved\"}"));
      return;
    }
    for (uint16_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
      if (strcmp(previousLongAddrs[i], panelMap_[i].longAddr) != 0) {
        clearLegacyLabelArtifacts(panelMap_[i].label);
      }
    }
    invalidateLegacyDiscovery();
    statusDirty_ = true;
    addEvent("optimizer config saved; count=%u", panelFieldCount_);
    String body = F("{\"ok\":true,\"panel_field_count\":");
    body += String(panelFieldCount_);
    body += F(",\"message\":\"optimizer configuration saved\"}");
    sendJson(body);
  }

  void handleApiEvents() {
    beginJsonStream();
    webServer_.sendContent("{\"events\":[");
    bool first = true;
    for (size_t i = 0; i < MAX_EVENTS; ++i) {
      const size_t idx = (recentEventHead_ + i) % MAX_EVENTS;
      if (events_[idx].text[0] == '\0') {
        continue;
      }
      const bool addComma = !first;
      first = false;
      char item[224];
      char escText[EVENT_TEXT_LEN * 2];
      jsonEscapeToBuffer(events_[idx].text, escText, sizeof(escText));
      snprintf(item, sizeof(item), "%s{\"ms\":%lu,\"text\":\"%s\"}",
               addComma ? "," : "", (unsigned long)events_[idx].ms, escText);
      item[sizeof(item) - 1] = '\0';
      webServer_.sendContent(item);
      platformYield();
    }
    webServer_.sendContent("]}");
    webServer_.sendContent("");
  }

  void handleApiInterestingFrames() {
    uint32_t sinceSeq = 0;
    if (webServer_.hasArg("since_seq")) {
      sinceSeq = (uint32_t)strtoul(webServer_.arg("since_seq").c_str(), nullptr, 0);
    }
    beginJsonStream();
    char head[128];
    snprintf(head, sizeof(head),
             "{\"head_seq\":%lu,\"dropped\":%lu,\"capacity\":%u,\"frames\":[",
             (unsigned long)interestingFrameSeq_,
             (unsigned long)interestingFrameDropped_,
             (unsigned)MAX_INTERESTING_FRAMES);
    head[sizeof(head) - 1] = '\0';
    webServer_.sendContent(head);
    bool first = true;
    for (size_t i = 0; i < MAX_INTERESTING_FRAMES; ++i) {
      const size_t idx = (interestingFrameHead_ + i) % MAX_INTERESTING_FRAMES;
      const InterestingFrame& frame = interestingFrames_[idx];
      if (!frame.valid || frame.seq <= sinceSeq) {
        continue;
      }
      char reasonEsc[INTERESTING_FRAME_REASON_LEN * 2];
      char payloadEsc[INTERESTING_FRAME_PAYLOAD_HEX_LEN * 2];
      jsonEscapeToBuffer(frame.reason, reasonEsc, sizeof(reasonEsc));
      jsonEscapeToBuffer(frame.payloadHex, payloadEsc, sizeof(payloadEsc));
      char item[640];
      snprintf(item, sizeof(item),
               "%s{\"seq\":%lu,\"ms\":%lu,\"frame_counter\":%lu,"
               "\"type_hex\":\"0x%04X\",\"type_name\":\"%s\","
               "\"gateway_id_hex\":\"0x%04X\",\"addr_raw_hex\":\"0x%04X\","
               "\"from_gateway\":%s,\"crc_ok\":%s,\"payload_len\":%u,"
               "\"payload_preview_hex\":\"%s\",\"payload_truncated\":%s,"
               "\"reason\":\"%s\"}",
               first ? "" : ",",
               (unsigned long)frame.seq,
               (unsigned long)frame.ms,
               (unsigned long)frame.frameCounter,
               frame.typeCode,
               frameTypeName(frame.typeCode),
               frame.gatewayId,
               frame.addrRaw,
               frame.fromGateway ? "true" : "false",
               frame.crcOk ? "true" : "false",
               (unsigned)frame.payloadLen,
               payloadEsc,
               frame.payloadTruncated ? "true" : "false",
               reasonEsc);
      item[sizeof(item) - 1] = '\0';
      webServer_.sendContent(item);
      first = false;
      platformYield();
    }
    webServer_.sendContent("]}");
    webServer_.sendContent("");
  }

  void handleApiInterestingFramesClear() {
    memset(interestingFrames_, 0, sizeof(interestingFrames_));
    interestingFrameHead_ = 0;
    interestingFrameDropped_ = 0;
    interestingFrameSeq_ = 0;
    lastInterestingGatewayId_ = 0;
    addEvent("interesting frame log cleared");
    sendJson(200, F("{\"ok\":true,\"message\":\"interesting frame log cleared\"}"));
  }

  void handleApiPolling() {
    beginJsonStream();
    webServer_.sendContent("{");
    bool firstField = true;
    sendJsonObjectFieldRaw(firstField, "enabled", activePollingEnabled_ ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "default_enabled", TIGO_RS485_ACTIVE_POLLING ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "awaiting_receive_response", awaitingReceiveResponse_ ? "true" : "false");
    char tmp[20];
    formatUnsigned32(TIGO_RS485_POLL_INTERVAL_MS, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "poll_interval_ms", tmp);
    formatUnsigned32(TIGO_RS485_POLL_TIMEOUT_MS, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "poll_timeout_ms", tmp);
    formatUnsigned16(countValidNodeMap(), tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "discovered_nodes", tmp);
    webServer_.sendContent("}");
    webServer_.sendContent("");
  }

  void handleApiPollingSet() {
    if (!webServer_.hasArg("enabled")) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"enabled is required\"}");
      return;
    }
    bool enabled = activePollingEnabled_;
    if (!otxParseBoolText(webServer_.arg("enabled").c_str(), activePollingEnabled_, &enabled, true)) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"enabled must be on/off/true/false/1/0/toggle\"}");
      return;
    }
    const bool changed = setActivePollingEnabled(enabled, "web");
    String body = F("{\"ok\":true,\"changed\":");
    body += changed ? F("true") : F("false");
    body += F(",\"enabled\":");
    body += activePollingEnabled_ ? F("true") : F("false");
    body += F("}");
    sendJson(body);
  }

  void handleApiPollingSeed() {
    if (!webServer_.hasArg("value") && !webServer_.hasArg("seed") && !webServer_.hasArg("packet")) {
      sendJson(400, F("{\"ok\":false,\"error\":\"value_required\",\"message\":\"set value=0x6040 or use seed/packet\"}"));
      return;
    }
    if (tapCommandActive()) {
      String body = F("{\"ok\":false,\"error\":\"busy\",\"message\":\"");
      body += jsonEscape(activeTapCommandStatus());
      body += F("\"}");
      sendJson(409, body);
      return;
    }

    const char* argName = webServer_.hasArg("value") ? "value" : (webServer_.hasArg("seed") ? "seed" : "packet");
    const uint16_t seed = parseQueryU16(argName, TIGO_CCA_POLL_SEED_FALLBACK);
    const uint16_t previous = nextPacketNumber_;
    nextPacketNumber_ = seed;
    lastRequestedPacketNumber_ = seed;
    awaitingReceiveResponse_ = false;
    const uint32_t now = platformMillis();
    lastPollSentMs_ = now > TIGO_RS485_POLL_INTERVAL_MS ? now - TIGO_RS485_POLL_INTERVAL_MS : 0;
    persistentStateDirty_ = true;
    persistentStateDirtySinceMs_ = now;
    statusDirty_ = true;
    addEvent("poll seed set via web: %04X -> %04X", previous, seed);

    String body = F("{\"ok\":true,\"previous_packet_hex\":\"");
    body += hex4(previous);
    body += F("\",\"next_packet_hex\":\"");
    body += hex4(nextPacketNumber_);
    body += F("\",\"fallback_hex\":\"");
    body += hex4(TIGO_CCA_POLL_SEED_FALLBACK);
    body += F("\"}");
    sendJson(body);
  }

  void handleApiMqttSettings() {
    beginJsonStream();
    webServer_.sendContent("{");
    bool firstField = true;
    sendJsonObjectFieldString(firstField, "host", mqttSettings_.host);
    char tmp[20];
    formatUnsigned16(mqttSettings_.port, tmp, sizeof(tmp));
    sendJsonObjectFieldRaw(firstField, "port", tmp);
    sendJsonObjectFieldString(firstField, "base_topic", mqttSettings_.baseTopic);
    sendJsonObjectFieldString(firstField, "username", mqttSettings_.username);
    sendJsonObjectFieldRaw(firstField, "password_set", mqttSettings_.password[0] ? "true" : "false");
    sendJsonObjectFieldRaw(firstField, "connected", mqttClient_.connected() ? "true" : "false");
    sendJsonObjectFieldString(firstField, "client_id", mqttClientId_);
    webServer_.sendContent("}");
    webServer_.sendContent("");
  }

  void handleApiMqttSettingsSave() {
    MqttRuntimeSettings next = mqttSettings_;
    if (!copyValidatedArg("host", next.host, sizeof(next.host), true, "MQTT host")) {
      return;
    }
    if (!copyValidatedArg("base_topic", next.baseTopic, sizeof(next.baseTopic), true, "MQTT base topic")) {
      return;
    }
    if (!copyValidatedArg("username", next.username, sizeof(next.username), false, "MQTT username")) {
      return;
    }
    if (webServer_.hasArg("password")) {
      if (!copyValidatedArg("password", next.password, sizeof(next.password), false, "MQTT password")) {
        return;
      }
    }
    const uint16_t port = parseQueryU16("port", mqttSettings_.port);
    if (port == 0) {
      sendJson(400, F("{\"ok\":false,\"error\":\"invalid_port\",\"message\":\"port must be 1..65535\"}"));
      return;
    }
    next.port = port;
    next.magic = TIGO_MQTT_SETTINGS_MAGIC;
    next.version = TIGO_MQTT_SETTINGS_VERSION;
    if (!topicLooksValid(next.baseTopic)) {
      sendJson(400, F("{\"ok\":false,\"error\":\"invalid_base_topic\",\"message\":\"base topic must not start/end with / or contain MQTT wildcards\"}"));
      return;
    }
    mqttSettings_ = next;
    if (!saveMqttSettingsToPersistentStore()) {
      sendJson(500, F("{\"ok\":false,\"error\":\"persist_failed\",\"message\":\"MQTT settings could not be saved\"}"));
      return;
    }
    mqttClient_.disconnect();
    setupMqtt();
    lastMqttConnectAttemptMs_ = 0;
    legacyDiscoveryPublished_ = false;
    legacyStateTopicsCleared_ = false;
    statusDirty_ = true;
    legacyStateDirty_ = true;
    addEvent("mqtt settings saved: %s:%u topic=%s",
             mqttSettings_.host,
             (unsigned)mqttSettings_.port,
             mqttSettings_.baseTopic);
    sendJson(F("{\"ok\":true,\"message\":\"MQTT settings saved\"}"));
  }

  void handleApiReboot() {
    addEvent("web reboot requested");
    rebootRequested_ = true;
    rebootAtMs_ = platformMillis() + 750UL;
    sendJson(F("{\"ok\":true,\"message\":\"reboot scheduled\"}"));
  }

  void handleApiWarmReboot() {
    if (!webServer_.hasArg("cursor")) {
      sendJson(400, F("{\"ok\":false,\"error\":\"cursor_required\",\"message\":\"cursor must be persisted, zero, or cca_bootstrap\"}"));
      return;
    }
    const String value = webServer_.arg("cursor");
    BootCursorStrategy selected = BootCursorStrategy::Persisted;
    if (value == "zero") {
      selected = BootCursorStrategy::Zero;
    } else if (value == "cca" || value == "cca_bootstrap") {
      selected = BootCursorStrategy::CcaBootstrap;
    } else if (value != "persisted") {
      sendJson(400, F("{\"ok\":false,\"error\":\"invalid_cursor\",\"message\":\"cursor must be persisted, zero, or cca_bootstrap\"}"));
      return;
    }
    bootCursorStrategy_ = selected;
    if (!saveBootOptionsToPersistentStore()) {
      sendJson(500, F("{\"ok\":false,\"error\":\"persist_failed\"}"));
      return;
    }
    addEvent("controlled read-only reboot requested cursor=%s", bootCursorStrategyName(selected));
    rebootRequested_ = true;
    rebootAtMs_ = platformMillis() + 750UL;
    String body = F("{\"ok\":true,\"message\":\"read-only warm reboot scheduled\",\"cursor_strategy\":\"");
    body += bootCursorStrategyName(selected);
    body += F("\"}");
    sendJson(body);
  }

  void handleApiReceiveBootstrap() {
    if (!webServer_.hasArg("confirm") ||
        webServer_.arg("confirm") != "RECEIVE_BOOTSTRAP") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\"}"));
      return;
    }
    if (!webServer_.hasArg("payload_hex")) {
      sendJson(400, F("{\"ok\":false,\"error\":\"payload_required\"}"));
      return;
    }
    uint8_t payload[5];
    size_t len = 0;
    const String text = webServer_.arg("payload_hex");
    if (!hexTextToBytes(text.c_str(), payload, sizeof(payload), &len) || len != sizeof(payload) ||
        (memcmp(payload, "\x00\x00\x00\x00\x00", 5) != 0 &&
         memcmp(payload, "\x00\x00\xEE\xEE\x00", 5) != 0)) {
      sendJson(400, F("{\"ok\":false,\"error\":\"unsupported_payload\",\"message\":\"only captured 0000000000 and 0000EEEE00 bootstraps are allowed\"}"));
      return;
    }
    if (!activePollingEnabled_ || gatewayId_ == 0 || tapCommandActive() || awaitingReceiveResponse_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"busy_or_unavailable\"}"));
      return;
    }
    lastRequestedPacketNumber_ = ((uint16_t)payload[2] << 8) | payload[3];
    const bool ok = sendBootReceiveSeed(payload, sizeof(payload), "manual response-aware receive bootstrap");
    queueTapCommandResponse("receive_bootstrap", ok,
                            ok ? "waiting for bootstrap 0x0149" : "command busy");
  }

  void handleApiReplaySession() {
    if (!webServer_.hasArg("confirm") ||
        webServer_.arg("confirm") != "TRACE_REPLAY") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\"}"));
      return;
    }
    const String mode = webServer_.hasArg("mode") ? webServer_.arg("mode") : "";
    if (traceReplayActive()) {
      sendJson(409, F("{\"ok\":false,\"error\":\"scheduled_trace_replay_active\"}"));
      return;
    }
    if (mode == "start") {
      if (!activePollingEnabled_ || gatewayId_ == 0 || tapCommandActive() ||
          awaitingReceiveResponse_) {
        sendJson(409, F("{\"ok\":false,\"error\":\"busy_or_unavailable\"}"));
        return;
      }
      finishNodeWakeSequence(false, "exclusive trace replay");
      replayExclusiveMode_ = true;
      recoveryAuthorized_ = true;
      addEvent("exclusive trace replay session started; automatic TAP traffic suspended");
      sendJson(F("{\"ok\":true,\"message\":\"exclusive replay session started\"}"));
      return;
    }
    if (mode == "stop") {
      abortTapCommand("exclusive replay session stopped");
      replayExclusiveMode_ = false;
      recoveryAuthorized_ = false;
      readOnlyWarmAttachProtectsState_ = true;
      if (bootJournal_.rollbackNeeded) {
        addEvent("exclusive replay stopped with unresolved address transaction; starting candidate recovery");
        beginReadOnlyWarmAttach(true);
      } else {
        lastPollSentMs_ = platformMillis() - TIGO_RS485_POLL_INTERVAL_MS;
        addEvent("exclusive trace replay session stopped; polling resumed");
      }
      sendJson(F("{\"ok\":true,\"message\":\"exclusive replay session stopped\"}"));
      return;
    }
    sendJson(400, F("{\"ok\":false,\"error\":\"mode_must_be_start_or_stop\"}"));
  }

  void handleApiTraceReplayStatus() {
    const uint32_t now = platformMillis();
    String body;
    body.reserve(1400);
    body += F("{\"state\":\""); body += traceReplayStateName(traceReplayState_);
    body += F("\",\"active\":"); body += traceReplayActive() ? F("true") : F("false");
    body += F(",\"exclusive\":"); body += replayExclusiveMode_ ? F("true") : F("false");
    body += F(",\"step_count\":"); body += String(traceReplayStepCount_);
    body += F(",\"step_index\":"); body += String(traceReplayStepIndex_);
    body += F(",\"started_ms\":"); body += String(traceReplayStartedMs_);
    body += F(",\"elapsed_ms\":");
    body += traceReplayStartedMs_ == 0 || timeBefore(now, traceReplayStartedMs_)
        ? String(0) : String(elapsed(now, traceReplayStartedMs_));
    body += F(",\"hold_until_offset_ms\":"); body += String(traceReplayHoldUntilOffsetMs_);
    body += F(",\"max_late_ms\":"); body += String(traceReplayMaxLateMs_);
    body += F(",\"poll_interval_ms\":"); body += String(traceReplayPollIntervalMs_);
    body += F(",\"receive_pump\":"); body += traceReplayReceivePumpEnabled_ ? F("true") : F("false");
    body += F(",\"polls_during_replay\":");
    body += String((traceReplayActive() ? pollsSent_ : traceReplayPollsAtEnd_) -
                   traceReplayPollsAtStart_);
    body += F(",\"failure_reason\":\""); body += jsonEscape(traceReplayFailureReason_);
    body += F("\",\"baseline\":{\"gateway_id_hex\":\""); body += hex4(traceReplayBaselineGatewayId_);
    body += F("\",\"node_table_hash\":\""); body += hex8(traceReplayBaselineNodeTableHash_);
    body += F("\",\"radio_fingerprint\":\""); body += hex8(traceReplayBaselineRadioFingerprint_);
    body += F("\",\"network_mode\":"); body += String(traceReplayBaselineNetworkMode_);
    body += F(",\"network_confirmed\":"); body += String(traceReplayBaselineNetworkConfirmed_);
    body += F(",\"network_expected\":"); body += String(traceReplayBaselineNetworkExpected_);
    body += F(",\"state_changing_tx\":"); body += String(traceReplayBaselineStateChangingTx_);
    body += F(",\"active_rf_tx\":"); body += String(traceReplayBaselineActiveRfTx_);
    body += F("},\"current\":{\"gateway_id_hex\":\""); body += hex4(gatewayId_);
    body += F("\",\"node_table_hash\":\""); body += hex8(currentNodeTableHash());
    body += F("\",\"radio_fingerprint\":\"");
    body += hex8(currentRadioDescriptorValid_
        ? fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) : 0);
    body += F("\",\"network_mode\":"); body += String(lastNetworkMode_);
    body += F(",\"network_confirmed\":"); body += String(lastNetworkConfirmedNodes_);
    body += F(",\"network_expected\":"); body += String(lastNetworkExpectedNodes_);
    body += F(",\"state_changing_tx\":"); body += String(destructiveManagementFramesTx_);
    body += F(",\"active_rf_tx\":"); body += String(activeRfFramesTx_);
    body += F("}}");
    sendJson(body);
  }

  void handleApiTraceReplayResult() {
    const uint16_t index = parseQueryU16("index", 0xFFFFU);
    if (index >= traceReplayStepCount_ || index >= TIGO_TRACE_REPLAY_MAX_STEPS) {
      sendJson(404, F("{\"ok\":false,\"error\":\"result_not_found\"}"));
      return;
    }
    const TraceReplayStep& step = traceReplaySteps_[index];
    const TraceReplayResult& result = traceReplayResults_[index];
    String body;
    body.reserve(900);
    body += F("{\"ok\":true,\"index\":"); body += String(index);
    body += F(",\"block\":"); body += String(step.block);
    body += F(",\"action\":\""); body += traceReplayActionName(step.action);
    body += F("\",\"risk\":\""); body += traceReplayRiskName(step.risk);
    body += F("\",\"expected_outcome\":\""); body += traceReplayOutcomeName(step.outcome);
    body += F("\",\"offset_ms\":"); body += String(step.offsetMs);
    body += F(",\"valid\":"); body += result.valid ? F("true") : F("false");
    body += F(",\"step_ok\":"); body += result.ok ? F("true") : F("false");
    body += F(",\"actual_outcome\":\""); body += traceReplayOutcomeName(result.actualOutcome);
    body += F("\",\"actual_start_offset_ms\":"); body += String(result.actualStartOffsetMs);
    body += F(",\"completed_offset_ms\":"); body += String(result.completedOffsetMs);
    body += F(",\"start_late_ms\":"); body += String(result.startLateMs);
    body += F(",\"interstep_late_ms\":"); body += String(result.interStepLateMs);
    body += F(",\"dispatch_preparation_ms\":"); body += String(result.dispatchPreparationMs);
    body += F(",\"response_type_hex\":\""); body += hex4(result.responseType);
    body += F("\",\"response_gateway_hex\":\""); body += hex4(result.responseGatewayId);
    char tmp[5];
    snprintf(tmp, sizeof(tmp), "%02X", result.responseDsn);
    body += F("\",\"response_dsn_hex\":\""); body += tmp;
    snprintf(tmp, sizeof(tmp), "%02X", result.responseSubcmd);
    body += F("\",\"response_subcmd_hex\":\""); body += tmp;
    snprintf(tmp, sizeof(tmp), "%02X", result.responseCredit);
    body += F("\",\"response_credit_hex\":\""); body += tmp;
    body += F("\"}");
    sendJson(body);
  }

  void handleApiTraceReplayPlan() {
    if (!webServer_.hasArg("confirm") ||
        webServer_.arg("confirm") != "TRACE_REPLAY") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\"}"));
      return;
    }
    const String mode = webServer_.hasArg("mode") ? webServer_.arg("mode") : "";
    if (mode == "clear") {
      if (traceReplayActive()) {
        sendJson(409, F("{\"ok\":false,\"error\":\"replay_active\"}"));
        return;
      }
      clearTraceReplayPlan();
      sendJson(F("{\"ok\":true,\"message\":\"trace replay plan cleared\"}"));
      return;
    }
    if (mode == "abort") {
      if (!traceReplayActive()) {
        sendJson(409, F("{\"ok\":false,\"error\":\"replay_not_active\"}"));
        return;
      }
      abortTraceReplay("API abort");
      sendJson(F("{\"ok\":true,\"message\":\"trace replay aborted\"}"));
      return;
    }
    if (mode == "append") {
      if (traceReplayActive() || traceReplayStepCount_ >= TIGO_TRACE_REPLAY_MAX_STEPS) {
        sendJson(409, F("{\"ok\":false,\"error\":\"active_or_plan_full\"}"));
        return;
      }
      const uint16_t requestedIndex = parseQueryU16("index", traceReplayStepCount_);
      if (requestedIndex < traceReplayStepCount_) {
        String duplicate = F("{\"ok\":true,\"index\":");
        duplicate += String(requestedIndex);
        duplicate += F(",\"already_appended\":true}");
        sendJson(duplicate);
        return;
      }
      if (requestedIndex != traceReplayStepCount_) {
        sendJson(409, F("{\"ok\":false,\"error\":\"append_index_mismatch\"}"));
        return;
      }
      TraceReplayStep step{};
      const String actionText = webServer_.hasArg("action") ? webServer_.arg("action") : "";
      if (actionText == "simple_frame") {
        step.action = TraceReplayAction::SimpleFrame;
      } else if (actionText == "receive_bootstrap") {
        step.action = TraceReplayAction::ReceiveBootstrap;
      } else if (actionText == "pv_subcommand") {
        step.action = TraceReplayAction::PvSubcommand;
      } else {
        sendJson(400, F("{\"ok\":false,\"error\":\"invalid_action\"}"));
        return;
      }
      const String outcomeText = webServer_.hasArg("outcome") ? webServer_.arg("outcome") : "response";
      if (outcomeText == "response") {
        step.outcome = TraceReplayOutcome::FrameResponse;
      } else if (outcomeText == "no_response") {
        step.outcome = TraceReplayOutcome::NoResponse;
      } else if (outcomeText == "tap_ack") {
        step.outcome = TraceReplayOutcome::TapAck;
      } else if (outcomeText == "rf_response") {
        step.outcome = TraceReplayOutcome::RfResponse;
      } else if (outcomeText == "local_tap_ack") {
        step.outcome = TraceReplayOutcome::LocalTapAck;
      } else {
        sendJson(400, F("{\"ok\":false,\"error\":\"invalid_outcome\"}"));
        return;
      }
      step.offsetMs = parseQueryU32("offset_ms", UINT32_MAX);
      step.targetGatewayId = parseQueryU16("target", 0xFFFFU);
      step.typeCode = parseQueryU16("type", 0);
      step.expectedType = parseQueryU16("expected_type", 0);
      step.rfNodeId = parseQueryU16("rf_node", 0);
      step.expectedPvSubcmd = (uint8_t)parseQueryU16("expected_subcmd", 0);
      step.expectedCredit = (uint8_t)parseQueryU16("expected_credit", 0xFFU);
      step.block = (uint8_t)parseQueryU16("block", 0);
      if (step.offsetMs == UINT32_MAX || step.expectedType == 0 ||
          (traceReplayStepCount_ > 0 &&
           step.offsetMs < traceReplaySteps_[traceReplayStepCount_ - 1U].offsetMs)) {
        sendJson(400, F("{\"ok\":false,\"error\":\"invalid_or_unsorted_timing\"}"));
        return;
      }
      if (webServer_.hasArg("payload_hex")) {
        const String payloadHex = webServer_.arg("payload_hex");
        size_t payloadLen = 0;
        if (!hexTextToBytes(payloadHex.c_str(), step.payload, sizeof(step.payload), &payloadLen)) {
          sendJson(400, F("{\"ok\":false,\"error\":\"invalid_payload\"}"));
          return;
        }
        step.payloadLen = (uint8_t)payloadLen;
      }
      if (step.action == TraceReplayAction::ReceiveBootstrap && step.payloadLen != 5) {
        sendJson(400, F("{\"ok\":false,\"error\":\"bootstrap_requires_5_bytes\"}"));
        return;
      }
      step.risk = traceReplayRiskFor(step.action, step.typeCode,
                                     step.payload, step.payloadLen);
      if (step.action == TraceReplayAction::PvSubcommand &&
          (step.typeCode & 0xFFU) == 0x0D && step.payloadLen > 2) {
        step.risk = TraceReplayRisk::StateChanging;
      }
      traceReplaySteps_[traceReplayStepCount_] = step;
      const uint8_t appended = traceReplayStepCount_++;
      traceReplayState_ = TraceReplayState::Loaded;
      String body = F("{\"ok\":true,\"index\":");
      body += String(appended);
      body += F(",\"risk\":\""); body += traceReplayRiskName(step.risk);
      body += F("\"}");
      sendJson(body);
      return;
    }
    if (mode == "start") {
      if (traceReplayStepCount_ == 0 || traceReplayActive() || replayExclusiveMode_ ||
          !activePollingEnabled_ || gatewayId_ == 0 || tapCommandActive() ||
          awaitingReceiveResponse_) {
        sendJson(409, F("{\"ok\":false,\"error\":\"empty_busy_or_unavailable\"}"));
        return;
      }
      if (strlen(gatewayLongAddr_) != 16) {
        sendJson(409, F("{\"ok\":false,\"error\":\"tap_identity_unavailable\"}"));
        return;
      }
      bool allowStateChanging = false;
      bool allowActiveRf = false;
      if (webServer_.hasArg("allow_state_changing") &&
          !otxParseBoolText(webServer_.arg("allow_state_changing").c_str(), false,
                            &allowStateChanging, false)) {
        sendJson(400, F("{\"ok\":false,\"error\":\"invalid_allow_state_changing\"}"));
        return;
      }
      if (webServer_.hasArg("allow_active_rf") &&
          !otxParseBoolText(webServer_.arg("allow_active_rf").c_str(), false,
                            &allowActiveRf, false)) {
        sendJson(400, F("{\"ok\":false,\"error\":\"invalid_allow_active_rf\"}"));
        return;
      }
      for (uint8_t i = 0; i < traceReplayStepCount_; ++i) {
        if (traceReplaySteps_[i].risk == TraceReplayRisk::StateChanging &&
            !allowStateChanging) {
          sendJson(403, F("{\"ok\":false,\"error\":\"state_changing_not_authorized\"}"));
          return;
        }
        if (traceReplaySteps_[i].risk == TraceReplayRisk::ActiveRf &&
            !allowActiveRf && !allowStateChanging) {
          sendJson(403, F("{\"ok\":false,\"error\":\"active_rf_not_authorized\"}"));
          return;
        }
        if (traceReplaySteps_[i].risk == TraceReplayRisk::Unknown &&
            !allowStateChanging) {
          sendJson(403, F("{\"ok\":false,\"error\":\"unknown_semantics_not_authorized\"}"));
          return;
        }
      }
      finishNodeWakeSequence(false, "scheduled trace replay");
      memset(traceReplayResults_, 0, sizeof(traceReplayResults_));
      traceReplayStepIndex_ = 0;
      traceReplayFailureReason_[0] = '\0';
      traceReplayReceivePumpEnabled_ = false;
      traceReplayUnexpectedResponse_ = false;
      traceReplayIdentityMismatch_ = false;
      traceReplayMaxLateMs_ = parseQueryU32("max_late_ms", 250UL);
      if (traceReplayMaxLateMs_ < 20UL) traceReplayMaxLateMs_ = 20UL;
      if (traceReplayMaxLateMs_ > 5000UL) traceReplayMaxLateMs_ = 5000UL;
      traceReplayPollIntervalMs_ = parseQueryU32("poll_interval_ms", 15UL);
      if (traceReplayPollIntervalMs_ < 5UL) traceReplayPollIntervalMs_ = 5UL;
      if (traceReplayPollIntervalMs_ > 250UL) traceReplayPollIntervalMs_ = 250UL;
      traceReplayPollGuardMs_ = parseQueryU32("poll_guard_ms", 8UL);
      const uint32_t startDelayMs = parseQueryU32("start_delay_ms", 250UL);
      traceReplayStartedMs_ = platformMillis() + startDelayMs;
      const uint32_t minimumHold = traceReplaySteps_[traceReplayStepCount_ - 1U].offsetMs;
      traceReplayHoldUntilOffsetMs_ = parseQueryU32("hold_until_ms", minimumHold);
      if (traceReplayHoldUntilOffsetMs_ < minimumHold) {
        traceReplayHoldUntilOffsetMs_ = minimumHold;
      }
      traceReplayPollsAtStart_ = pollsSent_;
      traceReplayPollsAtEnd_ = pollsSent_;
      copyString(traceReplayExpectedTapEui_, sizeof(traceReplayExpectedTapEui_), gatewayLongAddr_);
      traceReplayBaselineCursorMalformed_ = cursorMalformedPayloadCount_;
      traceReplayBaselineCursorDuplicate_ = cursorDuplicateResponseCount_;
      traceReplayBaselineCursorForwardResync_ = cursorForwardResyncCount_;
      traceReplayBaselineGatewayId_ = gatewayId_;
      traceReplayBaselineNodeTableHash_ = currentNodeTableHash();
      traceReplayBaselineRadioFingerprint_ = currentRadioDescriptorValid_
          ? fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) : 0;
      traceReplayBaselineNetworkMode_ = lastNetworkMode_;
      traceReplayBaselineNetworkConfirmed_ = lastNetworkConfirmedNodes_;
      traceReplayBaselineNetworkExpected_ = lastNetworkExpectedNodes_;
      traceReplayBaselineStateChangingTx_ = destructiveManagementFramesTx_;
      traceReplayBaselineActiveRfTx_ = activeRfFramesTx_;
      traceReplayAllowStateChanging_ = allowStateChanging;
      traceReplayAllowActiveRf_ = allowActiveRf;
      replayExclusiveMode_ = true;
      recoveryAuthorized_ = true;
      traceReplayState_ = TraceReplayState::Running;
      addEvent("scheduled trace replay armed steps=%u start_delay=%lums hold=%lums max_late=%lums poll=%lums",
               (unsigned)traceReplayStepCount_,
               (unsigned long)startDelayMs,
               (unsigned long)traceReplayHoldUntilOffsetMs_,
               (unsigned long)traceReplayMaxLateMs_,
               (unsigned long)traceReplayPollIntervalMs_);
      sendJson(F("{\"ok\":true,\"message\":\"trace replay armed on ESP\"}"));
      return;
    }
    sendJson(400, F("{\"ok\":false,\"error\":\"mode_must_be_clear_append_start_or_abort\"}"));
  }

  void handleApiBootJournal() {
    String body;
    body.reserve(4096);
    body += F("{\"tap_eui64\":\""); body += jsonEscape(bootJournal_.tapLongAddr);
    body += F("\",\"tap_firmware\":\""); body += jsonEscape(bootJournal_.tapFirmware);
    body += F("\",\"last_working_gateway_id_hex\":\""); body += hex4(bootJournal_.lastWorkingGatewayId);
    body += F("\",\"radio_descriptor_fingerprint\":\""); body += hex8(bootJournal_.radioDescriptorFingerprint);
    body += F("\",\"node_table_hash\":\""); body += hex8(bootJournal_.nodeTableHash);
    body += F("\",\"packet_cursor_hex\":\""); body += hex4(bootJournal_.packetCursor);
    body += F("\",\"last_power_epoch\":"); body += String(bootJournal_.lastPowerEpoch);
    body += F(",\"last_released_epoch\":"); body += String(bootJournal_.lastReleasedEpoch);
    body += F(",\"boot_path\":\""); body += tapBootPathName((TapBootPath)bootJournal_.lastBootPath);
    body += F("\",\"tap_state\":\""); body += tapObservedStateName((TapObservedState)bootJournal_.lastTapState);
    body += F("\",\"cursor_strategy\":\""); body += bootCursorStrategyName((BootCursorStrategy)bootJournal_.lastCursorStrategy);
    body += F("\",\"network\":{\"valid\":"); body += bootJournal_.networkStatusValid ? F("true") : F("false");
    body += F(",\"mode\":"); body += String(bootJournal_.networkMode);
    body += F(",\"countdown\":"); body += String(bootJournal_.networkCountdown);
    body += F(",\"flags\":"); body += String(bootJournal_.networkFlags);
    body += F(",\"confirmed\":"); body += String(bootJournal_.networkConfirmed);
    body += F(",\"expected\":"); body += String(bootJournal_.networkExpected);
    body += F("},\"address_transaction\":{\"state\":"); body += String(bootJournal_.transactionState);
    body += F(",\"type_hex\":\""); body += hex4(bootJournal_.transactionType);
    body += F("\",\"before_id_hex\":\""); body += hex4(bootJournal_.transactionBeforeId);
    body += F("\",\"requested_id_hex\":\""); body += hex4(bootJournal_.transactionRequestedId);
    body += F("\",\"confirmed_id_hex\":\""); body += hex4(bootJournal_.transactionConfirmedId);
    body += F("\",\"rollback_needed\":"); body += bootJournal_.rollbackNeeded ? F("true") : F("false");
    body += F("},\"mutations\":[");
    const uint8_t mutationStart = (uint8_t)((bootJournal_.mutationHead +
        TIGO_BOOT_MUTATION_JOURNAL_ENTRIES - bootJournal_.mutationCount) %
        TIGO_BOOT_MUTATION_JOURNAL_ENTRIES);
    for (uint8_t i = 0; i < bootJournal_.mutationCount; ++i) {
      if (i > 0) body += ',';
      const BootMutationEntry& entry = bootJournal_.mutations[
          (mutationStart + i) % TIGO_BOOT_MUTATION_JOURNAL_ENTRIES];
      body += F("{\"epoch\":"); body += String(entry.epoch);
      body += F(",\"type_hex\":\""); body += hex4(entry.typeCode);
      body += F("\",\"subcommand_hex\":\"");
      char subcmd[3]; snprintf(subcmd, sizeof(subcmd), "%02X", entry.subcommand); body += subcmd;
      body += F("\",\"result\":"); body += String(entry.result);
      body += F(",\"before_gateway_hex\":\""); body += hex4(entry.beforeGatewayId);
      body += F("\",\"requested_gateway_hex\":\""); body += hex4(entry.requestedGatewayId);
      body += F("\",\"confirmed_gateway_hex\":\""); body += hex4(entry.confirmedGatewayId);
      body += F("\",\"network_confirmed_before\":"); body += String(entry.beforeNetworkConfirmed);
      body += F(",\"network_confirmed_after\":"); body += String(entry.afterNetworkConfirmed);
      body += F(",\"node_table_hash_before\":\""); body += hex8(entry.beforeNodeTableHash);
      body += F("\",\"node_table_hash_after\":\""); body += hex8(entry.afterNodeTableHash);
      body += F("\"}");
    }
    body += F("],\"last_mutation\":\""); body += jsonEscape(bootJournal_.lastMutation);
    body += F("\"}");
    sendJson(body);
  }

  void handleApiPing() {
    queueTapCommandResponse("ping", ping(), tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiRsdControl() {
    const String mode = webServer_.hasArg("mode") ? webServer_.arg("mode") : "";
    const bool run = mode == "run";
    if (!run && mode != "stop") {
      sendJson(400, F("{\"ok\":false,\"error\":\"mode_must_be_run_or_stop\"}"));
      return;
    }
    const char* confirmation = run ? "RSD_RUN" : "RSD_STOP";
    if (!webServer_.hasArg("confirm") || webServer_.arg("confirm") != confirmation) {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\"}"));
      return;
    }
    const bool ok = requestRsdControl(run);
    queueTapCommandResponse(run ? "rsd_run" : "rsd_stop", ok,
                            tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiVersion() {
    queueTapCommandResponse("version", requestVersion(), tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiNodeTable() {
    queueTapCommandResponse("node_table", requestNodeTable(0), tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiNetworkStatus() {
    queueTapCommandResponse("network_status", requestNetworkStatus(), tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiRadioProfile() {
    char currentFingerprint[12];
    char workingFingerprint[12];
    char rollbackFingerprint[12];
    radioDescriptorFingerprint(currentRadioDescriptorValid_ ? currentRadioDescriptor_ : nullptr,
                               currentFingerprint, sizeof(currentFingerprint));
    radioDescriptorFingerprint(workingRadioDescriptorValid_ ? workingRadioDescriptor_ : nullptr,
                               workingFingerprint, sizeof(workingFingerprint));
    radioDescriptorFingerprint(rollbackRadioDescriptorValid_ ? rollbackRadioDescriptor_ : nullptr,
                               rollbackFingerprint, sizeof(rollbackFingerprint));
    const uint16_t currentChannel = currentRadioDescriptorValid_
        ? (((uint16_t)currentRadioDescriptor_[0] << 8) | currentRadioDescriptor_[1]) : 0;
    const uint16_t currentPan = currentRadioDescriptorValid_
        ? (((uint16_t)currentRadioDescriptor_[2] << 8) | currentRadioDescriptor_[3]) : 0;
    const uint16_t workingChannel = workingRadioDescriptorValid_
        ? (((uint16_t)workingRadioDescriptor_[0] << 8) | workingRadioDescriptor_[1]) : 0;
    const uint16_t workingPan = workingRadioDescriptorValid_
        ? (((uint16_t)workingRadioDescriptor_[2] << 8) | workingRadioDescriptor_[3]) : 0;
    const uint16_t rollbackChannel = rollbackRadioDescriptorValid_
        ? (((uint16_t)rollbackRadioDescriptor_[0] << 8) | rollbackRadioDescriptor_[1]) : 0;
    const uint16_t rollbackPan = rollbackRadioDescriptorValid_
        ? (((uint16_t)rollbackRadioDescriptor_[2] << 8) | rollbackRadioDescriptor_[3]) : 0;
    const bool mismatch = currentRadioDescriptorValid_ && workingRadioDescriptorValid_ &&
        memcmp(currentRadioDescriptor_, workingRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) != 0;
    const bool joinSeedMatches = radioJoinSeedValid_ && currentRadioDescriptorValid_ &&
        radioJoinSeedProfileFingerprint_ == fnv1a32(currentRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN);

    String body;
    body.reserve(960);
    body += F("{\"current\":{\"valid\":");
    body += currentRadioDescriptorValid_ ? F("true") : F("false");
    body += F(",\"channel\":"); body += String(currentChannel);
    body += F(",\"pan_id_hex\":\""); body += hex4(currentPan);
    body += F("\",\"fingerprint_fnv1a32\":\""); body += currentFingerprint;
    body += F("\",\"tap_long_addr\":\""); body += jsonEscape(currentRadioDescriptorTapLongAddr_);
    body += F("\",\"age_ms\":");
    body += String(currentRadioDescriptorValid_ ? elapsed(platformMillis(), currentRadioDescriptorUpdatedMs_) : 0);
    body += F("},\"working\":{\"valid\":");
    body += workingRadioDescriptorValid_ ? F("true") : F("false");
    body += F(",\"channel\":"); body += String(workingChannel);
    body += F(",\"pan_id_hex\":\""); body += hex4(workingPan);
    body += F("\",\"fingerprint_fnv1a32\":\""); body += workingFingerprint;
    body += F("\",\"tap_long_addr\":\""); body += jsonEscape(workingRadioDescriptorTapLongAddr_);
    body += F("\"},\"rollback\":{\"valid\":");
    body += rollbackRadioDescriptorValid_ ? F("true") : F("false");
    body += F(",\"channel\":"); body += String(rollbackChannel);
    body += F(",\"pan_id_hex\":\""); body += hex4(rollbackPan);
    body += F("\",\"fingerprint_fnv1a32\":\""); body += rollbackFingerprint;
    body += F("\",\"tap_long_addr\":\""); body += jsonEscape(rollbackRadioDescriptorTapLongAddr_);
    body += F("\"},\"profile_mismatch\":"); body += mismatch ? F("true") : F("false");
    body += F(",\"join_seed_saved\":"); body += radioJoinSeedValid_ ? F("true") : F("false");
    body += F(",\"join_seed_matches_current_profile\":"); body += joinSeedMatches ? F("true") : F("false");
    body += F(",\"descriptor_secret_redacted\":true}");
    sendJson(body);
  }

  void handleApiRadioProfileSaveCurrent() {
    if (!webServer_.hasArg("confirm") || webServer_.arg("confirm") != "SAVE_CURRENT") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\",\"message\":\"confirm=SAVE_CURRENT is required\"}"));
      return;
    }
    if (!currentRadioDescriptorValid_ || gatewayLongAddr_[0] == '\0' ||
        strcmp(currentRadioDescriptorTapLongAddr_, gatewayLongAddr_) != 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_current_profile\",\"message\":\"read and verify the current TAP radio profile first\"}"));
      return;
    }
    memcpy(workingRadioDescriptor_, currentRadioDescriptor_, sizeof(workingRadioDescriptor_));
    workingRadioDescriptorValid_ = true;
    copyString(workingRadioDescriptorTapLongAddr_, sizeof(workingRadioDescriptorTapLongAddr_), gatewayLongAddr_);
    if (!saveRadioIdentityToPersistentStore()) {
      sendJson(500, F("{\"ok\":false,\"error\":\"persist_failed\"}"));
      return;
    }
    addEvent("current radio profile explicitly saved as working for tap=%s", gatewayLongAddr_);
    sendJson(F("{\"ok\":true,\"message\":\"current radio profile saved as working\"}"));
  }

  void handleApiRadioProfileApplyWorking() {
    if (!webServer_.hasArg("confirm") ||
        webServer_.arg("confirm") != "APPLY_WORKING_RADIO_PROFILE") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\",\"message\":\"confirm=APPLY_WORKING_RADIO_PROFILE is required\"}"));
      return;
    }
    if (!activePollingEnabled_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"polling_disabled\"}"));
      return;
    }
    if (!workingRadioDescriptorValid_ || !currentRadioDescriptorValid_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"profile_missing\",\"message\":\"both a proven working profile and a current TAP readback are required\"}"));
      return;
    }
    if (gatewayLongAddr_[0] == '\0' ||
        strcmp(currentRadioDescriptorTapLongAddr_, gatewayLongAddr_) != 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"tap_identity_unverified\"}"));
      return;
    }
    const bool rewriteMatchingProfile =
        webServer_.hasArg("rewrite") && webServer_.arg("rewrite") == "true";
    const bool alreadyMatches =
        memcmp(currentRadioDescriptor_, workingRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) == 0;
    if (alreadyMatches && !rewriteMatchingProfile) {
      sendJson(409, F("{\"ok\":false,\"error\":\"already_matches\",\"message\":\"current TAP already has the working radio profile\"}"));
      return;
    }
    memcpy(rollbackRadioDescriptor_, currentRadioDescriptor_, sizeof(rollbackRadioDescriptor_));
    rollbackRadioDescriptorValid_ = true;
    copyString(rollbackRadioDescriptorTapLongAddr_, sizeof(rollbackRadioDescriptorTapLongAddr_),
               gatewayLongAddr_);
    if (!saveRadioIdentityToPersistentStore()) {
      rollbackRadioDescriptorValid_ = false;
      sendJson(500, F("{\"ok\":false,\"error\":\"rollback_persist_failed\",\"message\":\"profile write was not attempted\"}"));
      return;
    }
    const bool ok = writeGatewayRadioDescriptor(workingRadioDescriptor_);
    if (ok && alreadyMatches) {
      addEvent("explicit rewrite of matching working radio profile queued");
    }
    queueTapCommandResponse("radio_profile_apply", ok,
                             ok ? (alreadyMatches ? "matching working profile rewrite queued"
                                                  : "working profile write queued")
                                : "command busy");
  }

  void handleApiRadioProfileApplyRollback() {
    if (!webServer_.hasArg("confirm") ||
        webServer_.arg("confirm") != "APPLY_ROLLBACK_RADIO_PROFILE") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\",\"message\":\"confirm=APPLY_ROLLBACK_RADIO_PROFILE is required\"}"));
      return;
    }
    if (!activePollingEnabled_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"polling_disabled\"}"));
      return;
    }
    if (!rollbackRadioDescriptorValid_ || !currentRadioDescriptorValid_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"rollback_missing\"}"));
      return;
    }
    if (gatewayLongAddr_[0] == '\0' ||
        strcmp(currentRadioDescriptorTapLongAddr_, gatewayLongAddr_) != 0 ||
        strcmp(rollbackRadioDescriptorTapLongAddr_, gatewayLongAddr_) != 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"tap_identity_mismatch\"}"));
      return;
    }
    if (memcmp(currentRadioDescriptor_, rollbackRadioDescriptor_, TIGO_RADIO_DESCRIPTOR_LEN) == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"already_matches_rollback\"}"));
      return;
    }
    const bool ok = writeGatewayRadioDescriptor(rollbackRadioDescriptor_);
    queueTapCommandResponse("radio_profile_rollback", ok,
                            ok ? "rollback profile write queued" : "command busy");
  }

  void handleApiRadioConfig() {
    queueTapCommandResponse("radio_config", requestGatewayRadioConfig(), tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiEnumerate() {
    if (!webServer_.hasArg("confirm") ||
        webServer_.arg("confirm") != "ENUMERATE_TAP") {
      sendJson(400, F("{\"ok\":false,\"error\":\"enumeration_confirmation_required\"}"));
      return;
    }
    const uint16_t enumId = parseQueryU16("enumId", TIGO_ENUM_ID);
    const uint16_t desiredId = parseQueryU16("desiredId", TIGO_DESIRED_GATEWAY_ID);
    if (enumId == 0 || desiredId == 0) {
      sendJson(400, F("{\"ok\":false,\"error\":\"enum_and_desired_id_required\"}"));
      return;
    }
    queueTapCommandResponse("enumerate", enumerateGateway(enumId, desiredId), tapCommandActive() ? activeTapCommandStatus() : "command busy");
  }

  void handleApiSimpleFrame() {
    if (!webServer_.hasArg("type")) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"type is required\"}");
      return;
    }
    const uint16_t targetGatewayId = parseQueryU16("gatewayId", gatewayId_);
    const uint16_t typeCode = parseQueryU16("type", 0xFFFFU);
    if (typeCode == 0xFFFFU) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_type\"}");
      return;
    }

    uint8_t payload[64];
    size_t payloadLen = 0;
    if (webServer_.hasArg("payload_hex")) {
      const String payloadHex = webServer_.arg("payload_hex");
      if (!hexTextToBytes(payloadHex.c_str(), payload, sizeof(payload), &payloadLen)) {
        webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"payload_hex must contain an even number of hexadecimal digits and fit into 64 bytes\"}");
        return;
      }
    }

    bool waitForAck = webServer_.hasArg("expectedType");
    if (webServer_.hasArg("wait") &&
        !otxParseBoolText(webServer_.arg("wait").c_str(), waitForAck, &waitForAck, false)) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"wait must be true/false/on/off/1/0\"}");
      return;
    }

    const uint16_t expectedType = parseQueryU16("expectedType", 0);
    const uint16_t expectedGatewayId = parseQueryU16("expectedGatewayId", targetGatewayId);
    const uint16_t timeoutMs = parseQueryU16("timeoutMs", 800);
    const bool addressOrEnumMutation =
        typeCode == 0x0010 || typeCode == 0x0012 ||
        typeCode == 0x0014 || typeCode == 0x003C;
    if (addressOrEnumMutation) {
      if (webServer_.method() != HTTP_POST) {
        sendJson(405, F("{\"ok\":false,\"error\":\"management_mutation_requires_post\"}"));
        return;
      }
      if (typeCode == 0x0014 || typeCode == 0x003C) {
        sendJson(400, F("{\"ok\":false,\"error\":\"use_confirmed_enumerate_endpoint\"}"));
        return;
      }
      if (!webServer_.hasArg("confirm") ||
          webServer_.arg("confirm") != "ADDRESS_CHANGE") {
        sendJson(400, F("{\"ok\":false,\"error\":\"address_change_confirmation_required\"}"));
        return;
      }
      if (typeCode == 0x003C && payloadLen != 14) {
        sendJson(400, F("{\"ok\":false,\"error\":\"assign_frame_requires_14_byte_payload\"}"));
        return;
      }
      const uint16_t requiredAck =
          typeCode == 0x0010 ? 0x0011 :
          typeCode == 0x0012 ? 0x0013 :
          typeCode == 0x0014 ? 0x0015 : 0x003D;
      if (!waitForAck || expectedType != requiredAck) {
        sendJson(400, F("{\"ok\":false,\"error\":\"management_mutation_requires_verified_ack\"}"));
        return;
      }
    }
    const bool rsdControl = typeCode == 0x0B00 && payloadLen == 1 && payload[0] <= 0x01;
    if (rsdControl) {
      const char* confirmation = payload[0] == 0x01 ? "RSD_RUN" : "RSD_STOP";
      if (webServer_.method() != HTTP_POST ||
          !webServer_.hasArg("confirm") || webServer_.arg("confirm") != confirmation ||
          !waitForAck || expectedType != 0x0B01) {
        sendJson(400, F("{\"ok\":false,\"error\":\"rsd_control_requires_confirmation_and_0b01_ack\"}"));
        return;
      }
    }
    const TraceReplayRisk rawRisk = traceReplayRiskFor(
        TraceReplayAction::SimpleFrame, typeCode, payload, payloadLen);
    if (rawRisk == TraceReplayRisk::Unknown &&
        (!webServer_.hasArg("confirm") ||
         webServer_.arg("confirm") != "UNSAFE_RAW_FRAME" ||
         webServer_.method() != HTTP_POST)) {
      sendJson(400, F("{\"ok\":false,\"error\":\"unknown_frame_semantics_require_explicit_post_confirmation\"}"));
      return;
    }
    bool ok = false;
    if (waitForAck && typeCode == 0x003C && payloadLen >= 14) {
      const uint16_t desiredGatewayId = ((uint16_t)payload[12] << 8) | payload[13];
      ok = sendGatewayAssignFrameCommand(targetGatewayId, desiredGatewayId,
                                         "journaled simple-frame address assignment");
    } else if (waitForAck && (typeCode == 0x0010 || typeCode == 0x0012)) {
      ok = sendMagicHandshakeFrame(typeCode,
                                   typeCode == 0x0010 ? 0x0011 : 0x0013,
                                   "journaled simple-frame address handshake",
                                   targetGatewayId);
    } else {
      ok = waitForAck
          ? sendSimpleFrameCommandTo(targetGatewayId, typeCode, payload, payloadLen,
                                     expectedType, expectedGatewayId,
                                     "simple frame probe", timeoutMs)
          : sendSimpleFrameNow(targetGatewayId, typeCode, payload, payloadLen);
    }

    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"queued\":");
    body += (ok && waitForAck) ? F("true") : F("false");
    body += F(",\"gateway_id_hex\":\"");
    body += hex4(targetGatewayId);
    body += F("\",\"type_hex\":\"");
    body += hex4(typeCode);
    body += F("\",\"expected_type_hex\":\"");
    body += hex4(expectedType);
    body += F("\",\"expected_gateway_id_hex\":\"");
    body += hex4(expectedGatewayId);
    body += F("\",\"payload_hex\":\"");
    char payloadHexOut[129];
    bytesToHex(payload, payloadLen, payloadHexOut, sizeof(payloadHexOut));
    body += payloadHexOut;
    body += F("\",\"message\":\"");
    body += jsonEscape(ok ? (waitForAck ? activeTapCommandStatus() : "simple frame sent") : "command busy");
    body += F("\"}");
    sendJson(body);
  }

  void handleApiRewritePvConfig() {
    if (!TIGO_CCA_NODE_WAKE_SEQUENCE || !TIGO_CCA_NODE_WAKE_PV_CONFIG_FALLBACK) {
      sendJson(400, F("{\"ok\":false,\"error\":\"disabled\",\"message\":\"node wake pv-config sequence is disabled in firmware\"}"));
      return;
    }
    if (!activePollingEnabled_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"polling_disabled\",\"message\":\"enable polling before rewriting pv config\"}"));
      return;
    }
    if (gatewayId_ == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_gateway\",\"message\":\"gateway is not known yet\"}"));
      return;
    }
    if (nodeWakeActive_ || tapCommandActive() || awaitingReceiveResponse_) {
      String body = F("{\"ok\":false,\"error\":\"busy\",\"message\":\"");
      body += jsonEscape(nodeWakeActive_ ? "node wake sequence already active" : activeTapCommandStatus());
      body += F("\"}");
      sendJson(409, body);
      return;
    }
    const uint16_t nodeCount = countWakeCandidateNodes();
    if (nodeCount == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_nodes\",\"message\":\"no live node-map or saved optimizer config available\"}"));
      return;
    }
    recoveryAuthorized_ = true;
    tapBootPath_ = TapBootPath::TargetedRecovery;
    beginNodeWakeSequence(platformMillis(), true);
    statusDirty_ = true;
    String body = F("{\"ok\":true,\"queued\":true,\"message\":\"pv config rewrite sequence started\",\"node_count\":");
    body += String(nodeCount);
    body += F(",\"node_wake_step\":");
    body += String(nodeWakeStep_);
    body += F("}");
    sendJson(body);
  }

  void handleApiForceLearn() {
    if (tapCommandActive()) {
      sendJson(409, F("{\"ok\":false,\"error\":\"busy\",\"message\":\"wait for the current TAP command\"}"));
      return;
    }
    if (!radioJoinSeedMatchesCurrentProfile()) {
      sendJson(409, F("{\"ok\":false,\"error\":\"profile_seed_mismatch\",\"message\":\"a matching saved radio join seed is required\"}"));
      return;
    }
    recoveryAuthorized_ = true;
    tapBootPath_ = TapBootPath::TargetedRecovery;
    beginForcedLearnWindow(platformMillis());
    if (!tapCommandActive()) {
      sendJson(503, F("{\"ok\":false,\"error\":\"tap_command_not_queued\"}"));
      return;
    }
    String body = F("{\"ok\":true,\"queued\":true,\"message\":\"forced learn window started; saved join seed will repeat despite confirmed table\",\"duration_ms\":");
    body += String(TIGO_CCA_LEARN_COUNTDOWN_TIMEOUT_MS);
    body += F("}");
    sendJson(body);
  }

  void handleApiContinueWake() {
    if (tapCommandActive()) {
      sendJson(409, F("{\"ok\":false,\"error\":\"busy\",\"message\":\"wait for the current TAP command\"}"));
      return;
    }
    if (radioProfileMismatch()) {
      sendJson(409, F("{\"ok\":false,\"error\":\"profile_mismatch\",\"message\":\"current TAP radio profile differs from the working profile\"}"));
      return;
    }
    const uint16_t nodeCount = countWakeCandidateNodes();
    if (nodeCount == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_nodes\",\"message\":\"no confirmed nodes are available for wake continuation\"}"));
      return;
    }

    const uint32_t now = platformMillis();
    recoveryAuthorized_ = true;
    tapBootPath_ = TapBootPath::TargetedRecovery;
    nodeWakeActive_ = true;
    nodeWakeCompleted_ = false;
    nodeWakeLearnActive_ = false;
    nodeWakeLearnWaitCountdown_ = false;
    nodeWakeSkipLearn_ = true;
    nodeWakeLearnRestartAfterPvRun_ = false;
    forceLearnUntilMs_ = 0;
    nodeSeedState_ = NodeSeedState::Done;
    nodeSeedAwaitingCommand_ = false;
    nodeWakeConfigFallbackActive_ = false;
    nodeWakeForcePvConfig_ = true;
    nodeWakeStep_ = 0;
    nodeWakeNextActionMs_ = now;
    nodeWakeLearnStartedMs_ = 0;
    nodeWakeLearnLastNetworkStatusMs_ = 0;
    nodeWakeLearnLastJoinSeedMs_ = 0;
    nodeWakeLearnWarmupStep_ = 0;
    lastNodeWakeAttemptMs_ = now;

    if (!requestGatewayLearnCancel()) {
      finishNodeWakeSequence(false, "could not queue learn cancel");
      sendJson(503, F("{\"ok\":false,\"error\":\"tap_command_not_queued\"}"));
      return;
    }
    addEvent("manual wake continuation: learn cancel queued; nodes=%u", nodeCount);

    String body = F("{\"ok\":true,\"queued\":true,\"message\":\"learn cancel queued; CCA setup and confirmed-node wake will continue\",\"node_count\":");
    body += String(nodeCount);
    body += F("}");
    sendJson(body);
  }

  void handleApiHoldLearn() {
    if (!webServer_.hasArg("confirm") || webServer_.arg("confirm") != "HOLD_LEARN") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\",\"message\":\"confirm=HOLD_LEARN is required\"}"));
      return;
    }
    if (gatewayId_ == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_gateway\"}"));
      return;
    }
    if (tapCommandActive() || awaitingReceiveResponse_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"busy\",\"message\":\"wait for the current TAP command\"}"));
      return;
    }

    finishNodeWakeSequence(true, "manual learn hold");
    recoveryAuthorized_ = false;
    readOnlyWarmAttachProtectsState_ = true;
    nodeSeedState_ = NodeSeedState::Done;
    forceLearnAfterProfileWrite_ = false;
    if (!requestGatewayLearnCancel()) {
      nodeWakeCompleted_ = false;
      sendJson(503, F("{\"ok\":false,\"error\":\"tap_command_not_queued\"}"));
      return;
    }
    addEvent("manual learn hold queued; automatic retry deferred");
    sendJson(F("{\"ok\":true,\"queued\":true,\"message\":\"learn cancel queued; automatic learn retry held\"}"));
  }

  void handleApiFullCcaReplay() {
    if (!webServer_.hasArg("confirm") || webServer_.arg("confirm") != "FULL_CCA_REPLAY") {
      sendJson(400, F("{\"ok\":false,\"error\":\"confirmation_required\",\"message\":\"confirm=FULL_CCA_REPLAY is required\"}"));
      return;
    }
    if (!activePollingEnabled_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"polling_disabled\",\"message\":\"enable polling before replaying CCA startup\"}"));
      return;
    }
    if (gatewayId_ == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_gateway\"}"));
      return;
    }
    if (tapCommandActive() || awaitingReceiveResponse_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"busy\",\"message\":\"wait for the current TAP command\"}"));
      return;
    }

    finishNodeWakeSequence(true, "manual full CCA replay");
    recoveryAuthorized_ = true;
    readOnlyWarmAttachProtectsState_ = false;
    warmAttachPhase_ = WarmAttachPhase::Idle;
    tapBootPath_ = TapBootPath::LegacyCcaReplay;
    bootJournal_.lastBootPath = (uint8_t)tapBootPath_;
    copyString(bootJournal_.lastMutation, sizeof(bootJournal_.lastMutation), "manual full CCA replay");
    markBootJournalDirty();
    forceLearnAfterProfileWrite_ = false;
    bootEnumeratePending_ = false;
    bootVersionPending_ = false;
    bootRadioConfigPending_ = false;
    bootGatewaySelector0Pending_ = false;
    bootNetworkStatusPending_ = false;
    bootNodeTablePending_ = false;
    bootGatewaySelector1Pending_ = false;
    bootNodeTableEndPending_ = false;
    bootCcaStep_ = 0;
    bootCcaWaitingStep_ = UINT8_MAX;
    bootCcaRetryCount_ = 0;
    bootCcaNextActionMs_ = platformMillis();
    bootCcaWarmAttachTried_ = true;
    bootCcaWarmAttachPending_ = false;
    statusDirty_ = true;
    addEvent("manual full CCA replay queued; warm attach bypassed once; table/profile preserved");
    sendJson(F("{\"ok\":true,\"queued\":true,\"message\":\"full CCA startup replay queued; table and radio profile preserved\"}"));
  }

  void handleApiRebuildNodeTable() {
    if (!activePollingEnabled_) {
      sendJson(409, F("{\"ok\":false,\"error\":\"polling_disabled\",\"message\":\"enable polling before rebuilding the TAP node table\"}"));
      return;
    }
    if (gatewayId_ == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_gateway\",\"message\":\"gateway is not known yet\"}"));
      return;
    }
    if (tapCommandActive() || awaitingReceiveResponse_) {
      String body = F("{\"ok\":false,\"error\":\"busy\",\"message\":\"");
      body += jsonEscape(activeTapCommandStatus());
      body += F("\"}");
      sendJson(409, body);
      return;
    }
    if (radioProfileMismatch()) {
      sendJson(409, F("{\"ok\":false,\"error\":\"profile_mismatch\",\"message\":\"current TAP radio profile differs from the saved working profile\"}"));
      return;
    }
    const uint16_t panelCount = countPanelMapLongAddrs();
    if (panelCount == 0) {
      sendJson(409, F("{\"ok\":false,\"error\":\"no_panels\",\"message\":\"configure optimizer serial numbers before rebuilding the TAP node table\"}"));
      return;
    }

    const uint32_t now = platformMillis();
    recoveryAuthorized_ = true;
    tapBootPath_ = TapBootPath::TargetedRecovery;
    if (nodeWakeActive_) {
      addEvent("manual node-table rebuild supersedes active wake/learn sequence");
    }
    nodeWakeActive_ = true;
    nodeWakeCompleted_ = false;
    nodeWakeLearnActive_ = false;
    nodeWakeLearnWaitCountdown_ = false;
    nodeWakeSkipLearn_ = false;
    nodeWakeLearnRestartAfterPvRun_ = false;
    nodeWakeConfigFallbackActive_ = false;
    nodeWakeForcePvConfig_ = true;
    nodeWakeStep_ = 0;
    nodeWakeLearnStartedMs_ = 0;
    nodeWakeLearnLastNetworkStatusMs_ = 0;
    nodeWakeLearnLastJoinSeedMs_ = 0;
    nodeWakeLearnWarmupStep_ = 0;
    forceLearnUntilMs_ = 0;
    lastNodeWakeAttemptMs_ = now;
    beginNodeSeedRecovery(now);
    statusDirty_ = true;
    addEvent("manual destructive node-table rebuild requested; panels=%u", panelCount);

    String body = F("{\"ok\":true,\"queued\":true,\"message\":\"TAP node table clear, exact pending seed, readback verification, and learn queued\",\"panel_count\":");
    body += String(panelCount);
    body += F("}");
    sendJson(body);
  }

  void handleApiPvConfigSet() {
    if (!webServer_.hasArg("nodeId") || !webServer_.hasArg("periodSlots") || !webServer_.hasArg("phaseSlots")) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"nodeId, periodSlots and phaseSlots are required\"}");
      return;
    }
    const uint16_t nodeId = parseQueryU16("nodeId", 0);
    const uint16_t periodSlots = parseQueryU16("periodSlots", 0);
    const uint16_t phaseSlots = parseQueryU16("phaseSlots", 0);
    const uint8_t reportType = (uint8_t)parseQueryU16("reportType", 0x31);
    const bool ok = setPvConfig(nodeId, periodSlots, phaseSlots, reportType);
    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"queued\":");
    body += ok ? F("true") : F("false");
    body += F(",\"node_id\":"); body += String(nodeId);
    body += F(",\"period_slots\":"); body += String(periodSlots);
    body += F(",\"phase_slots\":"); body += String(phaseSlots);
    body += F(",\"message\":\"");
    body += jsonEscape(ok ? activeTapCommandStatus() : "command busy");
    body += F("\"}");
    sendJson(body);
  }

  void handleApiPvSubcmd() {
    if (!webServer_.hasArg("subcmd")) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"subcmd is required\"}");
      return;
    }
    const uint16_t subcmdValue = parseQueryU16("subcmd", 0xFFFFU);
    if (subcmdValue > 0xFFU) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"subcmd must be 0..255\"}");
      return;
    }
    uint8_t payload[TIGO_MAX_PV_COMMAND_PAYLOAD];
    size_t payloadLen = 0;
    if (webServer_.hasArg("body_hex")) {
      const String bodyHex = webServer_.arg("body_hex");
      if (!hexTextToBytes(bodyHex.c_str(), payload, sizeof(payload), &payloadLen)) {
        webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"body_hex must contain an even number of hexadecimal digits and fit into the configured PV command payload limit\"}");
        return;
      }
    }
    const bool ok = sendPvSubcommand((uint8_t)subcmdValue, payload, payloadLen);
    // A manual probe must not replace the profile-bound seed learned while
    // passively observing a CCA. In particular, old-site 0x41 payloads can be
    // syntactically valid while belonging to a different RF identity.
    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"queued\":");
    body += ok ? F("true") : F("false");
    body += F(",\"subcmd_hex\":\"");
    char subcmdHex[5];
    snprintf(subcmdHex, sizeof(subcmdHex), "%02X", (unsigned)subcmdValue);
    subcmdHex[sizeof(subcmdHex) - 1] = '\0';
    body += subcmdHex;
    body += F("\",\"request_hex\":\"");
    body += jsonEscape(lastPvSubcommandRequestHex_);
    body += F("\",\"message\":\"");
    body += jsonEscape(ok ? activeTapCommandStatus() : "command busy");
    body += F("\"}");
    sendJson(body);
  }

  void handleApiNodeText() {
    if (!webServer_.hasArg("nodeId") || !webServer_.hasArg("text")) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"nodeId and text are required\"}");
      return;
    }
    bool appendCr = true;
    if (webServer_.hasArg("appendCr") &&
        !otxParseBoolText(webServer_.arg("appendCr").c_str(), true, &appendCr, false)) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"appendCr must be true/false/on/off/1/0\"}");
      return;
    }
    const uint16_t nodeId = parseQueryU16("nodeId", 0);
    const String text = webServer_.arg("text");
    const bool ok = sendNodeTextCommand(nodeId, text.c_str(), appendCr);
    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"queued\":");
    body += ok ? F("true") : F("false");
    body += F(",\"subcmd_hex\":\"06\",\"node_id\":");
    body += String(nodeId);
    body += F(",\"text\":\"");
    body += jsonEscape(text);
    body += F("\",\"append_cr\":");
    body += appendCr ? F("true") : F("false");
    body += F(",\"request_hex\":\"");
    body += jsonEscape(lastPvSubcommandRequestHex_);
    body += F("\",\"message\":\"");
    body += jsonEscape(ok ? activeTapCommandStatus() : "command busy");
    body += F("\"}");
    sendJson(body);
  }

  void handleApiNodeOp() {
    if (!webServer_.hasArg("subcmd") || !webServer_.hasArg("nodeId")) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"subcmd and nodeId are required\"}");
      return;
    }
    const uint16_t subcmdValue = parseQueryU16("subcmd", 0xFFFFU);
    if (subcmdValue > 0xFFU) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"subcmd must be 0..255\"}");
      return;
    }
    const uint16_t nodeId = parseQueryU16("nodeId", 0);
    const uint16_t opcodeValue = parseQueryU16("opcode", 0x0FU);
    if (opcodeValue > 0xFFU) {
      webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"opcode must be 0..255\"}");
      return;
    }
    const bool ok = sendNodeOpcodeCommand((uint8_t)subcmdValue, nodeId, (uint8_t)opcodeValue);
    String body = F("{\"ok\":");
    body += ok ? F("true") : F("false");
    body += F(",\"queued\":");
    body += ok ? F("true") : F("false");
    body += F(",\"subcmd_hex\":\"");
    char hexBuf[5];
    snprintf(hexBuf, sizeof(hexBuf), "%02X", (unsigned)subcmdValue);
    hexBuf[sizeof(hexBuf) - 1] = '\0';
    body += hexBuf;
    body += F("\",\"node_id\":");
    body += String(nodeId);
    body += F(",\"opcode_hex\":\"");
    snprintf(hexBuf, sizeof(hexBuf), "%02X", (unsigned)opcodeValue);
    hexBuf[sizeof(hexBuf) - 1] = '\0';
    body += hexBuf;
    body += F("\",\"request_hex\":\"");
    body += jsonEscape(lastPvSubcommandRequestHex_);
    body += F("\",\"message\":\"");
    body += jsonEscape(ok ? activeTapCommandStatus() : "command busy");
    body += F("\"}");
    sendJson(body);
  }

  uint16_t parseQueryU16(const char* name, uint16_t fallback) const {
    if (!webServer_.hasArg(name)) {
      return fallback;
    }
    const String s = webServer_.arg(name);
    char* endp = nullptr;
    const unsigned long value = strtoul(s.c_str(), &endp, 0);
    if (endp == s.c_str()) {
      return fallback;
    }
    return (uint16_t)(value & 0xFFFFU);
  }

  uint32_t parseQueryU32(const char* name, uint32_t fallback) const {
    if (!webServer_.hasArg(name)) {
      return fallback;
    }
    const String s = webServer_.arg(name);
    char* endp = nullptr;
    const unsigned long value = strtoul(s.c_str(), &endp, 0);
    if (endp == s.c_str() || *endp != '\0') {
      return fallback;
    }
    return (uint32_t)value;
  }

  // --------------------------
  // RS485 transport
  // --------------------------
  void setTransmitMode(bool tx) {
    platformRuntime_.rs485Port().setTransmitMode(tx);
  }

  bool sendGatewayFrame(uint16_t gatewayId, uint16_t typeCode, const uint8_t* payload, size_t payloadLen) {
    uint8_t body[MAX_FRAME_BODY];
    const size_t dataLen = 4 + payloadLen;
    if (dataLen + 2 > sizeof(body)) {
      addEvent("tx frame too large");
      return false;
    }
    if (typeCode == 0x0010 || typeCode == 0x0012 ||
        typeCode == 0x0014 || typeCode == 0x003C) {
      ++destructiveManagementFramesTx_;
      char label[32];
      snprintf(label, sizeof(label), "management frame 0x%04X", typeCode);
      recordMutationRequested(typeCode, 0, gatewayId, label);
      // State-changing frames use a write-ahead journal: the request record
      // must be durable before RS485 transmission begins.
      if (!flushBootJournal(true)) {
        addEvent("management frame %04X blocked: write-ahead journal unavailable", typeCode);
        finishMutationJournal(false);
        return false;
      }
    }

    body[0] = (uint8_t)(gatewayId >> 8);
    body[1] = (uint8_t)(gatewayId & 0xFF);
    body[2] = (uint8_t)(typeCode >> 8);
    body[3] = (uint8_t)(typeCode & 0xFF);
    if (payloadLen > 0 && payload != nullptr) {
      memcpy(body + 4, payload, payloadLen);
    }
    const uint16_t crc = crc16Tigo(body, dataLen);
    body[dataLen] = (uint8_t)(crc & 0xFF);
    body[dataLen + 1] = (uint8_t)(crc >> 8);

    PlatformRs485Port& rs485Port = platformRuntime_.rs485Port();
    Stream& rs485Stream = rs485Port.stream();
    lastGatewayFrameTxMs_ = platformMillis();
    ++gatewayFrameTxGeneration_;
    rs485Port.setTransmitMode(true);
    platformDelayMicroseconds(60);
    rs485Stream.write((uint8_t)0x00);
    rs485Stream.write((uint8_t)0xFF);
    rs485Stream.write((uint8_t)0xFF);
    rs485Stream.write((uint8_t)0x7E);
    rs485Stream.write((uint8_t)0x07);
    for (size_t i = 0; i < dataLen + 2; ++i) {
      writeEscapedByte(rs485Stream, body[i]);
    }
    rs485Stream.write((uint8_t)0x7E);
    rs485Stream.write((uint8_t)0x08);
    rs485Port.flushTransmit();
    platformDelayMicroseconds(100);
    rs485Port.setTransmitMode(false);
    rs485Port.prepareReceive();
    // Preserve full-duplex diagnostics without synchronously publishing while
    // the TAP reply is in flight. MQTT is flushed after RS485 is idle.
    queueRawFrameCapture(gatewayId, (uint16_t)(gatewayId & 0x7FFFU),
                         typeCode, false, true, lastGatewayFrameTxMs_,
                         payload, payloadLen);
    return true;
  }

  uint8_t nextSequence() {
    ++sequence_;
    if (sequence_ == 0x00 || sequence_ == 0xFF) {
      ++sequence_;
    }
    if (sequence_ == 0x00 || sequence_ == 0xFF) {
      sequence_ = 1;
    }
    return sequence_;
  }

  void sendReceiveRequest() {
    if (gatewayId_ == 0) {
      return;
    }
    uint8_t payload[5];
    payload[0] = 0x00;
    payload[1] = 0x01;
    payload[2] = (uint8_t)(nextPacketNumber_ >> 8);
    payload[3] = (uint8_t)(nextPacketNumber_ & 0xFF);
    payload[4] = 0x04;

    lastRequestedPacketNumber_ = nextPacketNumber_;
    if (!sendGatewayFrame(gatewayId_, 0x0148, payload, sizeof(payload))) {
      addEvent("poll blocked before transmit");
      return;
    }
    awaitingReceiveResponse_ = true;
    lastPollSentMs_ = lastGatewayFrameTxMs_;
    ++pollsSent_;
    if (TIGO_USB_ENABLE_FRAME_LOG) {
      Serial.printf("OTX tx_poll t=%lu gateway=%04X packet=%04X polls=%lu\r\n",
                    (unsigned long)lastPollSentMs_,
                    gatewayId_,
                    lastRequestedPacketNumber_,
                    (unsigned long)pollsSent_);
    }
  }

  // --------------------------
  // Serial receive / processing
  // --------------------------
  void serviceSerial() {
    GatewayFrame frame;
    uint16_t processed = 0;
    PlatformRs485Port& rs485Port = platformRuntime_.rs485Port();
    rs485Port.prepareReceive();
    Stream& rs485Stream = rs485Port.stream();
    while (rs485Stream.available() > 0 && processed < TIGO_RS485_MAX_RX_BYTES_PER_LOOP) {
      const int ch = rs485Stream.read();
      if (ch < 0) {
        break;
      }
      ++processed;
      if (parser_.feed((uint8_t)ch, frame)) {
        processFrame(frame);
      }
    }
  }

  uint8_t passiveResponseSubcmdForRequest(uint8_t requestSubcmd) const {
    switch (requestSubcmd) {
      case 0x0D: return 0x0E;
      case 0x26: return 0x27;
      case 0x2D:
      case 0x2E: return 0x2F;
      default: return 0;
    }
  }

  void rememberPassivePvExpectation(uint16_t gatewayId, uint8_t requestSubcmd, uint8_t dsn) {
    const uint8_t responseSubcmd = passiveResponseSubcmdForRequest(requestSubcmd);
    if (responseSubcmd == 0) {
      return;
    }
    const uint32_t now = platformMillis();
    size_t selected = MAX_PASSIVE_PV_EXPECTATIONS;
    for (size_t i = 0; i < MAX_PASSIVE_PV_EXPECTATIONS; ++i) {
      if (passivePvExpectations_[i].valid &&
          passivePvExpectations_[i].gatewayId == gatewayId &&
          passivePvExpectations_[i].responseSubcmd == responseSubcmd) {
        selected = i;
        break;
      }
      if (selected == MAX_PASSIVE_PV_EXPECTATIONS &&
          (!passivePvExpectations_[i].valid ||
           elapsed(now, passivePvExpectations_[i].observedMs) > PASSIVE_PV_EXPECTATION_TIMEOUT_MS)) {
        selected = i;
      }
    }
    if (selected == MAX_PASSIVE_PV_EXPECTATIONS) {
      selected = 0;
      for (size_t i = 1; i < MAX_PASSIVE_PV_EXPECTATIONS; ++i) {
        if (elapsed(now, passivePvExpectations_[i].observedMs) >
            elapsed(now, passivePvExpectations_[selected].observedMs)) {
          selected = i;
        }
      }
    }
    PassivePvExpectation& expectation = passivePvExpectations_[selected];
    expectation.valid = true;
    expectation.responseSubcmd = responseSubcmd;
    expectation.dsn = dsn;
    expectation.gatewayId = gatewayId;
    expectation.observedMs = now;
  }

  bool responseMatchesActiveCommandForObserver(const GatewayFrame& frame,
                                               const ActivePvEnvelope& env) const {
    if (!tapCommandActive() || commandState_.expectedType != 0x0B10 ||
        commandState_.expectedGatewayId != frame.gatewayId ||
        commandState_.expectedDsn != env.dsn ||
        commandState_.expectedPvResponseSubcmd != env.subcmd) {
      return false;
    }
    if (env.subcmd == 0x0E &&
        (commandState_.arg0 == 1 || commandState_.arg0 == 3) &&
        commandState_.expectedRadioDescriptorValid &&
        (env.bodyLen < TIGO_RADIO_DESCRIPTOR_LEN ||
         memcmp(env.body, commandState_.expectedRadioDescriptor,
                TIGO_RADIO_DESCRIPTOR_LEN) != 0)) {
      return false;
    }
    return true;
  }

  bool consumePassivePvExpectation(const GatewayFrame& frame, const ActivePvEnvelope& env) {
    const uint32_t now = platformMillis();
    for (size_t i = 0; i < MAX_PASSIVE_PV_EXPECTATIONS; ++i) {
      PassivePvExpectation& expectation = passivePvExpectations_[i];
      if (!expectation.valid) {
        continue;
      }
      if (elapsed(now, expectation.observedMs) > PASSIVE_PV_EXPECTATION_TIMEOUT_MS) {
        expectation.valid = false;
        continue;
      }
      if (expectation.gatewayId == frame.gatewayId &&
          expectation.responseSubcmd == env.subcmd && expectation.dsn == env.dsn) {
        expectation.valid = false;
        return true;
      }
    }
    return false;
  }

  void observeProtocolState(const GatewayFrame& frame) {
    if (frame.fromGateway && (frame.typeCode == 0x0039 || frame.typeCode == 0x003B) &&
        frame.payloadLen >= 10) {
      char observedLongAddr[17];
      bytesToHex(frame.payload, 8, observedLongAddr, sizeof(observedLongAddr));
      if (strcmp(gatewayLongAddr_, observedLongAddr) != 0) {
        learnTapAddressFromEnumInfo(frame.payload, frame.payloadLen,
                                    frame.typeCode == 0x0039 ? "0039-observer" : "003B-observer",
                                    frame.gatewayId);
      }
    }

    ActivePvEnvelope env{};
    if ((frame.typeCode != 0x0B0F && frame.typeCode != 0x0B10) ||
        !decodeActivePvEnvelope(frame.payload, frame.payloadLen, env)) {
      return;
    }
    if (!frame.fromGateway && frame.typeCode == 0x0B0F) {
      if (frame.gatewayId != gatewayId_) {
        return;
      }
      rememberPassivePvExpectation(frame.gatewayId, env.subcmd, env.dsn);
      if (env.subcmd == 0x41) {
        storeJoinSeed(env.body, env.bodyLen);
      }
      return;
    }
    if (!frame.fromGateway || frame.typeCode != 0x0B10 ||
        frame.gatewayId != gatewayId_) {
      return;
    }
    if (!responseMatchesActiveCommandForObserver(frame, env) &&
        !consumePassivePvExpectation(frame, env)) {
      return;
    }
    switch (env.subcmd) {
      case 0x0E:
        if (env.bodyLen >= TIGO_RADIO_DESCRIPTOR_LEN &&
            (env.bodyLen == TIGO_RADIO_DESCRIPTOR_LEN ||
             env.body[TIGO_RADIO_DESCRIPTOR_LEN] == 0x00 ||
             env.body[TIGO_RADIO_DESCRIPTOR_LEN] == 0x01)) {
          storeCurrentRadioDescriptor(env.body, env.bodyLen, "0x0E");
        }
        break;
      case 0x27:
        decodeAndStoreNodeTable(env.body, env.bodyLen);
        break;
      case 0x2F:
        decodeAndStoreNetworkStatus(env.body, env.bodyLen);
        break;
      default:
        break;
    }
  }

  void processFrame(const GatewayFrame& frame) {
    if (!frame.valid) {
      return;
    }
    ++framesRx_;
    lastFrameValid_ = true;
    lastFrameCrcOk_ = frame.crcOk;
    lastFrameFromGateway_ = frame.fromGateway;
    lastFrameGatewayId_ = frame.gatewayId;
    lastFrameAddrRaw_ = frame.addrRaw;
    lastFrameTypeCode_ = frame.typeCode;
    lastFramePayloadLen_ = frame.payloadLen;
    const size_t previewLen = frame.payloadLen > 64 ? 64 : frame.payloadLen;
    bytesToHex(frame.payload, previewLen, lastFramePayloadPreviewHex_, sizeof(lastFramePayloadPreviewHex_));
    lastFramePayloadTruncated_ = frame.payloadLen > previewLen;
    lastFrameMs_ = platformMillis();
    maybeRecordInterestingFrame(frame, lastFrameMs_, framesRx_);
    if (TIGO_USB_ENABLE_FRAME_LOG) {
      Serial.printf("OTX rx_frame t=%lu type=%04X gateway=%04X crc=%s from_gateway=%s payload_len=%u frames=%lu\r\n",
                    (unsigned long)lastFrameMs_,
                    frame.typeCode,
                    frame.gatewayId,
                    frame.crcOk ? "ok" : "bad",
                    frame.fromGateway ? "yes" : "no",
                    (unsigned)frame.payloadLen,
                    (unsigned long)framesRx_);
    }
    publishRawFrame(frame);
    if (!frame.crcOk) {
      ++framesCrcError_;
      addEvent("crc error type=%04X", frame.typeCode);
      return;
    }

    observeProtocolState(frame);
    observeTraceReplayFrame(frame);

    // A valid response to any command proves that the TAP is still reachable.
    // Keep this separate from tapResponsesRx_, which intentionally tracks only
    // regular 0x0149 receive-poll responses.
    if (frame.fromGateway && frame.gatewayId == gatewayId_) {
      lastTapResponseMs_ = lastFrameMs_;
    }

    if (handleTapCommandFrame(frame)) {
      return;
    }

    if (frame.typeCode == 0x0149 && frame.gatewayId == gatewayId_) {
      lastTapResponseMs_ = platformMillis();
      if (processReceiveResponse(frame)) {
        ++tapResponsesRx_;
        awaitingReceiveResponse_ = false;
      }
      return;
    }

    if (frame.typeCode == 0x000B) {
      char txt[96];
      size_t n = frame.payloadLen;
      if (n >= sizeof(txt)) {
        n = sizeof(txt) - 1;
      }
      memcpy(txt, frame.payload, n);
      txt[n] = '\0';
      sanitizeInlineText(txt);
      copyString(webVersionText_, sizeof(webVersionText_), txt);
      return;
    }
  }

  void maybeRecordInterestingFrame(const GatewayFrame& frame, uint32_t now, uint32_t frameCounter) {
    char reason[INTERESTING_FRAME_REASON_LEN];
    reason[0] = '\0';
    const bool gatewayChanged =
        (lastInterestingGatewayId_ != 0 && frame.gatewayId != 0 && frame.gatewayId != lastInterestingGatewayId_);

    if (!frame.crcOk) {
      copyString(reason, sizeof(reason), "crc_error");
    } else if (gatewayChanged) {
      copyString(reason, sizeof(reason), "gateway_changed");
    } else if (frame.typeCode == 0x0148) {
      if (frame.payloadLen != 5) {
        copyString(reason, sizeof(reason), "nonstandard_rx_poll");
      }
    } else if (frame.typeCode == 0x0149) {
      if (frame.payloadLen > 12) {
        copyString(reason, sizeof(reason), "long_rx_response");
      } else if (frame.payloadLen != 5 && frame.payloadLen != 12) {
        copyString(reason, sizeof(reason), "nonstandard_rx_response");
      }
    } else if (frame.typeCode == 0x0B0F) {
      snprintf(reason, sizeof(reason), "pv_subcmd_0x%02X", activePvRequestSubcmd(frame.payload, frame.payloadLen));
    } else if (frame.typeCode == 0x0B10) {
      ActivePvEnvelope env{};
      if (decodeActivePvEnvelope(frame.payload, frame.payloadLen, env)) {
        snprintf(reason, sizeof(reason), "pv_ack_0x%02X", env.subcmd);
      } else {
        copyString(reason, sizeof(reason), "pv_subcmd_ack");
      }
    } else if (isManagementFrameType(frame.typeCode)) {
      copyString(reason, sizeof(reason), frameTypeName(frame.typeCode));
    } else {
      copyString(reason, sizeof(reason), "unknown_type");
    }

    if (frame.gatewayId != 0) {
      lastInterestingGatewayId_ = frame.gatewayId;
    }
    if (reason[0] == '\0') {
      return;
    }
    recordInterestingFrame(frame, now, frameCounter, reason);
  }

  uint8_t activePvRequestSubcmd(const uint8_t* payload, size_t payloadLen) const {
    if (payload == nullptr || payloadLen < 4) {
      return 0;
    }
    return payload[3];
  }

  bool isManagementFrameType(uint16_t typeCode) const {
    switch (typeCode) {
      case 0x0014:
      case 0x0015:
      case 0x0038:
      case 0x0039:
      case 0x003A:
      case 0x003B:
      case 0x003C:
      case 0x003D:
      case 0x000A:
      case 0x000B:
      case 0x000E:
      case 0x000F:
      case 0x0052:
      case 0x0053:
      case 0x0E02:
      case 0x0006:
      case 0x0B00:
      case 0x0B01:
        return true;
      default:
        return false;
    }
  }

  void recordInterestingFrame(const GatewayFrame& frame,
                              uint32_t now,
                              uint32_t frameCounter,
                              const char* reason) {
    InterestingFrame& item = interestingFrames_[interestingFrameHead_];
    if (item.valid) {
      ++interestingFrameDropped_;
    }
    item.valid = true;
    item.crcOk = frame.crcOk;
    item.fromGateway = frame.fromGateway;
    item.seq = ++interestingFrameSeq_;
    item.ms = now;
    item.frameCounter = frameCounter;
    item.addrRaw = frame.addrRaw;
    item.gatewayId = frame.gatewayId;
    item.typeCode = frame.typeCode;
    item.payloadLen = (uint16_t)frame.payloadLen;
    const size_t previewLen = frame.payloadLen > INTERESTING_FRAME_PAYLOAD_PREVIEW_BYTES
        ? INTERESTING_FRAME_PAYLOAD_PREVIEW_BYTES
        : frame.payloadLen;
    item.payloadTruncated = frame.payloadLen > previewLen;
    bytesToHex(frame.payload, previewLen, item.payloadHex, sizeof(item.payloadHex));
    copyString(item.reason, sizeof(item.reason), reason ? reason : "interesting");
    interestingFrameHead_ = (uint8_t)((interestingFrameHead_ + 1U) % MAX_INTERESTING_FRAMES);
  }

  bool processReceiveResponse(const GatewayFrame& frame) {
    const uint32_t now = platformMillis();
    RxStatus status;
    const uint8_t* blob = nullptr;
    size_t blobLen = 0;
    if (!parseRxStatusAndBlob(frame.payload, frame.payloadLen, status, blob, blobLen)) {
      ++cursorMalformedPayloadCount_;
      addEvent("rx resp parse failed");
      return false;
    }

    size_t packetCount = 0;
    size_t powerPacketCount = 0;
    size_t idx = 0;
    while (idx + 7 <= blobLen) {
      const uint8_t ptype = blob[idx + 0];
      const uint8_t dataLen = blob[idx + 6];
      const size_t end = idx + 7 + dataLen;
      if (end > blobLen) {
        break;
      }
      if (ptype == 0x31) {
        ++powerPacketCount;
      }
      idx = end;
      ++packetCount;
    }

    const uint16_t echoedPacket = composeResponsePacketNumber(status);
    if (idx != blobLen) {
      ++cursorMalformedPayloadCount_;
      addEvent("0x0149 packet tail malformed consumed=%u blob=%u echoed=%04X",
               (unsigned)idx, (unsigned)blobLen, echoedPacket);
      return false;
    }
    if (echoedPacket != lastRequestedPacketNumber_) {
      const uint16_t forward = (uint16_t)(echoedPacket - lastRequestedPacketNumber_);
      if (forward < 0x8000U) {
        ++cursorForwardResyncCount_;
        cursorForwardDistance_ += forward;
        addEvent("cursor forward resync requested=%04X echoed=%04X distance=%u packets=%u",
                 lastRequestedPacketNumber_, echoedPacket, forward, (unsigned)packetCount);
      } else {
        ++cursorDuplicateResponseCount_;
        addEvent("cursor response behind request requested=%04X echoed=%04X distance=%u",
                 lastRequestedPacketNumber_, echoedPacket,
                 (unsigned)(uint16_t)(lastRequestedPacketNumber_ - echoedPacket));
        return false;
      }
    }
    if (firstReceiveResponseMs_ == 0) {
      firstReceiveResponseMs_ = now;
      addEvent("first valid 0x0149 after boot elapsed=%lums request=%04X len=%u",
               (unsigned long)elapsed(firstReceiveResponseMs_, bootStartedMs_),
               lastRequestedPacketNumber_, (unsigned)frame.payloadLen);
    }
    idx = 0;
    while (idx + 7 <= blobLen) {
      const uint8_t ptype = blob[idx + 0];
      const uint16_t nodeId = ((uint16_t)blob[idx + 1] << 8) | blob[idx + 2];
      const uint16_t shortAddr = ((uint16_t)blob[idx + 3] << 8) | blob[idx + 4];
      const uint8_t dsn = blob[idx + 5];
      const uint8_t dataLen = blob[idx + 6];
      const uint8_t* data = blob + idx + 7;
      handlePvPacket(ptype, nodeId, shortAddr, dsn, status.slotCounterGateway, data, dataLen);
      idx += 7 + dataLen;
    }
    nextPacketNumber_ = (uint16_t)(echoedPacket + packetCount);
    checkpointConfirmedCursor(nextPacketNumber_, now);
    maybePrintSerialRxPollStatus(status, blobLen, packetCount, powerPacketCount, echoedPacket);
    return true;
  }

  bool parseRxStatusAndBlob(const uint8_t* payload, size_t payloadLen, RxStatus& status, const uint8_t*& blob, size_t& blobLen) {
    if (payloadLen < 5) {
      return false;
    }
    memset(&status, 0, sizeof(status));
    size_t idx = 0;
    status.statusType = ((uint16_t)payload[idx] << 8) | payload[idx + 1];
    idx += 2;

    auto take = [&](size_t count, const uint8_t*& ptr) -> bool {
      if (idx + count > payloadLen) {
        return false;
      }
      ptr = payload + idx;
      idx += count;
      return true;
    };

    const uint8_t* ptr = nullptr;
    if ((status.statusType & 0x0001U) == 0) {
      if (!take(1, ptr)) return false;
      status.rxBuffersUsed = ptr[0];
      status.hasRxBuffersUsed = true;
    }
    if ((status.statusType & 0x0002U) == 0) {
      if (!take(1, ptr)) return false;
      status.txBuffersFree = ptr[0];
      status.hasTxBuffersFree = true;
    }
    if ((status.statusType & 0x0004U) == 0) {
      if (!take(2, ptr)) return false;
    }
    if ((status.statusType & 0x0008U) == 0) {
      if (!take(2, ptr)) return false;
    }
    if ((status.statusType & 0x0010U) == 0) {
      if (!take(1, ptr)) return false;
      status.packetCounterHigh = ptr[0];
      status.hasPacketCounterHigh = true;
    }
    if (!take(1, ptr)) return false;
    status.packetCounterLow = ptr[0];
    if (!take(2, ptr)) return false;
    status.slotCounterGateway = ((uint16_t)ptr[0] << 8) | ptr[1];

    blob = payload + idx;
    blobLen = payloadLen - idx;
    return true;
  }

  uint16_t composeResponsePacketNumber(const RxStatus& status) const {
    uint8_t high = status.hasPacketCounterHigh ? status.packetCounterHigh : (uint8_t)(lastRequestedPacketNumber_ >> 8);
    return ((uint16_t)high << 8) | status.packetCounterLow;
  }

  void handlePvPacket(uint8_t ptype, uint16_t nodeId, uint16_t shortAddr, uint8_t dsn, uint16_t gatewaySlotCounter,
                      const uint8_t* data, size_t dataLen) {
    (void)dsn;
    const uint16_t normalizedNodeId = normalizeNodeId(nodeId);
    switch (ptype) {
      case 0x31:
        markNodeConfirmed(normalizedNodeId);
        decodeAndStorePower(normalizedNodeId, shortAddr, gatewaySlotCounter, data, dataLen);
        break;
      case 0x27:
        decodeAndStoreNodeTable(data, dataLen);
        break;
      case 0x09:
        decodeAndStoreTopology(data, dataLen);
        break;
      case 0x0E:
      case 0x18:
      default:
        break;
    }
  }

  void decodeAndStorePower(uint16_t nodeId, uint16_t shortAddr, uint16_t gatewaySlotCounter, const uint8_t* data, size_t len) {
    if (len != 13) {
      return;
    }

    const uint16_t vinRaw = (uint16_t)(((uint16_t)data[0] << 4) | (data[1] >> 4)) & 0x0FFFU;
    const uint16_t voutRaw = (uint16_t)((((uint16_t)data[1] & 0x0FU) << 8) | data[2]) & 0x0FFFU;
    const uint8_t dutyRaw = data[3];
    const uint16_t iinRaw = (uint16_t)(((uint16_t)data[4] << 4) | (data[5] >> 4)) & 0x0FFFU;
    const uint16_t tempRaw = (uint16_t)((((uint16_t)data[5] & 0x0FU) << 8) | data[6]) & 0x0FFFU;

    const float vinV = vinRaw * 0.05f;
    const float voutV = voutRaw * 0.10f;
    const float iinA = iinRaw * TIGO_IIN_SCALE;
    const float tempC = tempRaw * 0.10f;
    const uint16_t slotCounterReport = ((uint16_t)data[10] << 8) | data[11];
    const uint8_t rssi = data[12];

    PowerReport* slot = findOrCreatePowerSlot(nodeId, shortAddr);
    if (slot == nullptr) {
      return;
    }
    const bool wasValid = slot->valid;

    slot->valid = true;
    slot->nodeId = nodeId;
    slot->shortAddr = shortAddr;
    slot->gatewaySlotCounter = gatewaySlotCounter;
    slot->slotCounterReport = slotCounterReport;
    slot->vinV = vinV;
    slot->voutV = voutV;
    slot->iinA = iinA;
    slot->tempC = tempC;
    slot->powerInW = vinV * iinA;
    slot->dutyPct = (dutyRaw / 255.0f) * 100.0f;
    slot->rssi = rssi;
    snprintf(slot->unknownHex, sizeof(slot->unknownHex), "%02x%02x%02x", data[7], data[8], data[9]);
    slot->updatedMs = platformMillis();
    if (firstPowerFrameMs_ == 0) {
      firstPowerFrameMs_ = slot->updatedMs;
      addEvent("first power frame after boot node=%u elapsed=%lums vout=%.2fV",
               nodeId, (unsigned long)elapsed(firstPowerFrameMs_, bootStartedMs_), voutV);
    }
    if (voutV >= TIGO_ELECTRICAL_RELEASE_MIN_VOUT_V) {
      if (firstReleasedPowerEvidenceMs_ == 0 ||
          lastReleasedPowerEvidenceMs_ == 0 ||
          elapsed(slot->updatedMs, lastReleasedPowerEvidenceMs_) > TIGO_SAMPLE_FRESH_MS) {
        firstReleasedPowerEvidenceMs_ = slot->updatedMs;
        releasedPowerEvidenceCount_ = 0;
      }
      lastReleasedPowerEvidenceMs_ = slot->updatedMs;
      ++releasedPowerEvidenceCount_;
    } else if (!hasFreshReleasedPower(slot->updatedMs)) {
      firstReleasedPowerEvidenceMs_ = 0;
      lastReleasedPowerEvidenceMs_ = 0;
      releasedPowerEvidenceCount_ = 0;
    }
    if (ntpTimeValid()) {
      const uint32_t epoch = (uint32_t)time(nullptr);
      bool journalChanged = false;
      if (bootJournal_.lastPowerEpoch == 0 ||
          epoch - bootJournal_.lastPowerEpoch >= TIGO_BOOT_JOURNAL_POWER_CHECKPOINT_S) {
        bootJournal_.lastPowerEpoch = epoch;
        journalChanged = true;
      }
      if (voutV >= TIGO_ELECTRICAL_RELEASE_MIN_VOUT_V &&
          (bootJournal_.lastReleasedEpoch == 0 ||
           epoch - bootJournal_.lastReleasedEpoch >= TIGO_BOOT_JOURNAL_POWER_CHECKPOINT_S)) {
        bootJournal_.lastReleasedEpoch = epoch;
        journalChanged = true;
      }
      if (journalChanged) {
        markBootJournalDirty(slot->updatedMs);
      }
    }
    // Power reports arrive continuously. Keep the MQTT summary/status cadence
    // bounded by the configured timers instead of republishing on every packet.
    legacyStateDirty_ = true;
    if (!wasValid) {
      invalidateLegacyDiscovery();
    }

    const char* longAddr = lookupLongAddrForNode(nodeId);
    if (longAddr) {
      copyString(slot->longAddr, sizeof(slot->longAddr), longAddr);
      const char* label = lookupPanelLabel(longAddr);
      copyString(slot->panelLabel, sizeof(slot->panelLabel), label ? label : "");
    } else {
      slot->longAddr[0] = '\0';
      slot->panelLabel[0] = '\0';
    }
    dedupePowerSlots();
    maybePrintSerialPowerStatus(slot->nodeId, slot->powerInW);
    markCurrentRadioDescriptorWorking();
    // Publish on the periodic MQTT cadence instead of once per packet.
    // This keeps the wireless and MQTT load bounded even on busy networks.
  }

  void maybePrintSerialPowerStatus(uint16_t lastNodeId, float lastPowerInW) {
    if (TIGO_SERIAL_POWER_STATUS_EVERY_MS == 0) {
      return;
    }
    const uint32_t now = platformMillis();
    if (lastSerialPowerStatusMs_ != 0 &&
        elapsed(now, lastSerialPowerStatusMs_) < TIGO_SERIAL_POWER_STATUS_EVERY_MS) {
      return;
    }
    lastSerialPowerStatusMs_ = now;
    const AggregatePowerStatus agg = otxBuildAggregatePowerStatus(
        powerSlots_, MAX_POWER_SLOTS, now, TIGO_SAMPLE_FRESH_MS, TIGO_SAMPLE_HOLD_MS);
    Serial.printf("OTX power t=%lu nodes=%u fresh=%u stale=%u expired=%u live_w=%.3f held_w=%.3f last_node=%u last_w=%.3f\r\n",
                  (unsigned long)now,
                  (unsigned)countValidPower(),
                  (unsigned)agg.freshNodes,
                  (unsigned)agg.staleNodes,
                  (unsigned)agg.expiredNodes,
                  agg.liveSumInputW,
                  agg.heldSumInputW,
                  (unsigned)lastNodeId,
                  lastPowerInW);
  }

  void maybePrintSerialRxPollStatus(const RxStatus& status,
                                    size_t blobLen,
                                    size_t packetCount,
                                    size_t powerPacketCount,
                                    uint16_t echoedPacket) {
    if (TIGO_SERIAL_RX_POLL_STATUS_EVERY_MS == 0) {
      return;
    }
    const uint32_t now = platformMillis();
    if (lastSerialRxPollStatusMs_ != 0 &&
        elapsed(now, lastSerialRxPollStatusMs_) < TIGO_SERIAL_RX_POLL_STATUS_EVERY_MS) {
      return;
    }
    lastSerialRxPollStatusMs_ = now;
    Serial.printf("OTX rx_poll t=%lu status=%04X blob=%u packets=%u power=%u slot=%u echoed=%04X next=%04X polls=%lu timeouts=%lu\r\n",
                  (unsigned long)now,
                  status.statusType,
                  (unsigned)blobLen,
                  (unsigned)packetCount,
                  (unsigned)powerPacketCount,
                  status.slotCounterGateway,
                  echoedPacket,
                  nextPacketNumber_,
                  (unsigned long)pollsSent_,
                  (unsigned long)pollTimeouts_);
  }

  void decodeAndStoreNodeTable(const uint8_t* data, size_t len) {
    if (len < 4) {
      return;
    }
    lastNodeTableStart_ = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t entryCount = ((uint16_t)data[2] << 8) | data[3];
    const size_t expectedLen = 4U + ((size_t)entryCount * 10U);
    if (expectedLen != len) {
      addEvent("node table malformed start=%u count=%u len=%u expected=%u",
               lastNodeTableStart_, entryCount, (unsigned)len, (unsigned)expectedLen);
      return;
    }
    lastNodeTableEntryCount_ = entryCount;
    lastNodeTableMs_ = platformMillis();
    if (lastNodeTableStart_ == 0 || lastNodeTableStart_ == TIGO_NODE_ID_BASE) {
      memset(nodeMap_, 0, sizeof(nodeMap_));
    }
    addEvent("node table response start=%u count=%u len=%u",
             lastNodeTableStart_,
             lastNodeTableEntryCount_,
             (unsigned)len);
    size_t pos = 4;
    for (uint16_t i = 0; i < entryCount; ++i) {
      if (pos + 10 > len) {
        break;
      }
      char longAddr[17];
      bytesToHex(data + pos, 8, longAddr, sizeof(longAddr));
      const uint16_t rawNodeId = ((uint16_t)data[pos + 8] << 8) | data[pos + 9];
      upsertNodeMap(rawNodeId, longAddr);
      pos += 10;
    }
    bootJournal_.nodeTableHash = currentNodeTableHash();
    bootJournal_.lastTapState = (uint8_t)classifyTapState(platformMillis());
    markBootJournalDirty();
  }

  void decodeAndStoreNetworkStatus(const uint8_t* data, size_t len) {
    if (data == nullptr || len < 9) {
      return;
    }
    lastNetworkStatusValid_ = true;
    lastNetworkStatusMs_ = platformMillis();
    lastNetworkMode_ = data[0];
    lastNetworkCountdown_ = ((uint16_t)data[1] << 8) | data[2];
    lastNetworkFlags_ = ((uint16_t)data[3] << 8) | data[4];
    lastNetworkConfirmedNodes_ = ((uint16_t)data[5] << 8) | data[6];
    lastNetworkExpectedNodes_ = ((uint16_t)data[7] << 8) | data[8];
    bootJournal_.networkStatusValid = 1;
    bootJournal_.networkMode = lastNetworkMode_;
    bootJournal_.networkCountdown = lastNetworkCountdown_;
    bootJournal_.networkFlags = lastNetworkFlags_;
    bootJournal_.networkConfirmed = lastNetworkConfirmedNodes_;
    bootJournal_.networkExpected = lastNetworkExpectedNodes_;
    bootJournal_.lastTapState = (uint8_t)classifyTapState(platformMillis());
    markBootJournalDirty();
    statusDirty_ = true;
    addEvent("network status mode=%u countdown=%u flags=%04X confirmed=%u expected=%u",
             (unsigned)lastNetworkMode_,
             lastNetworkCountdown_,
             lastNetworkFlags_,
             lastNetworkConfirmedNodes_,
             lastNetworkExpectedNodes_);
  }

  void decodeAndStoreTopology(const uint8_t* data, size_t len) {
    if (len < 16) {
      return;
    }
    const uint16_t nodeId = ((uint16_t)data[2] << 8) | data[3];
    char longAddr[17];
    bytesToHex(data + 8, 8, longAddr, sizeof(longAddr));
    upsertNodeMap(nodeId, longAddr);
    markNodeConfirmed(nodeId);
  }

  void upsertNodeMap(uint16_t rawNodeId, const char* longAddr) {
    const uint16_t nodeId = normalizeNodeId(rawNodeId);
    NodeMapEntry* slot = findOrCreateNodeMap(nodeId);
    if (slot == nullptr) {
      return;
    }
    const bool pending = (rawNodeId & 0x8000U) != 0;
    const bool sameIdentity =
        slot->valid && slot->nodeId == nodeId &&
        strcmp(slot->longAddr, longAddr) == 0;
    bool changed = !slot->valid || slot->rawNodeId != rawNodeId ||
                   strcmp(slot->longAddr, longAddr) != 0;
    if (!sameIdentity) {
      slot->rfConfirmed = false;
    }
    slot->valid = true;
    slot->pending = pending;
    slot->nodeId = nodeId;
    slot->rawNodeId = rawNodeId;
    copyString(slot->longAddr, sizeof(slot->longAddr), longAddr);
    slot->updatedMs = platformMillis();
    statusDirty_ = true;

    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (!powerSlots_[i].valid || powerSlots_[i].nodeId != nodeId) {
        continue;
      }
      copyString(powerSlots_[i].longAddr, sizeof(powerSlots_[i].longAddr), longAddr);
      const char* label = lookupPanelLabel(longAddr);
      copyString(powerSlots_[i].panelLabel, sizeof(powerSlots_[i].panelLabel), label ? label : "");
    }
    if (changed) {
      invalidateLegacyDiscovery();
    }
  }

  void markNodeConfirmed(uint16_t nodeId) {
    nodeId = normalizeNodeId(nodeId);
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid || nodeMap_[i].nodeId != nodeId) {
        continue;
      }
      const bool firstRfProof = !nodeMap_[i].rfConfirmed;
      nodeMap_[i].rfConfirmed = true;
      if (nodeMap_[i].pending || nodeMap_[i].rawNodeId != nodeId) {
        nodeMap_[i].pending = false;
        nodeMap_[i].rawNodeId = nodeId;
        nodeMap_[i].updatedMs = platformMillis();
        statusDirty_ = true;
        addEvent("node %u confirmed by RF telemetry", nodeId);
      } else if (firstRfProof) {
        nodeMap_[i].updatedMs = platformMillis();
        statusDirty_ = true;
        addEvent("node %u RF identity confirmed", nodeId);
      }
      return;
    }
  }

  uint8_t powerSlotMetadataScore(const PowerReport& slot) const {
    uint8_t score = 0;
    if (slot.longAddr[0] != '\0') {
      score = (uint8_t)(score + 2U);
    }
    if (slot.panelLabel[0] != '\0') {
      score = (uint8_t)(score + 1U);
    }
    if (slot.shortAddr != 0) {
      score = (uint8_t)(score + 1U);
    }
    return score;
  }

  bool shouldPreferPowerSlot(const PowerReport& current, const PowerReport& candidate) const {
    if (current.updatedMs != candidate.updatedMs) {
      return elapsed(candidate.updatedMs, current.updatedMs) < 0x80000000UL;
    }
    if (current.gatewaySlotCounter != candidate.gatewaySlotCounter) {
      return candidate.gatewaySlotCounter > current.gatewaySlotCounter;
    }
    if (current.slotCounterReport != candidate.slotCounterReport) {
      return candidate.slotCounterReport > current.slotCounterReport;
    }
    const uint8_t currentScore = powerSlotMetadataScore(current);
    const uint8_t candidateScore = powerSlotMetadataScore(candidate);
    if (currentScore != candidateScore) {
      return candidateScore > currentScore;
    }
    return candidate.shortAddr < current.shortAddr;
  }

  void mergePowerSlotMetadata(PowerReport* dst, const PowerReport& src) {
    if (dst == nullptr || !src.valid) {
      return;
    }
    if (dst->longAddr[0] == '\0' && src.longAddr[0] != '\0') {
      copyString(dst->longAddr, sizeof(dst->longAddr), src.longAddr);
    }
    if (dst->panelLabel[0] == '\0' && src.panelLabel[0] != '\0') {
      copyString(dst->panelLabel, sizeof(dst->panelLabel), src.panelLabel);
    }
    if (dst->unknownHex[0] == '\0' && src.unknownHex[0] != '\0') {
      copyString(dst->unknownHex, sizeof(dst->unknownHex), src.unknownHex);
    }
  }

  void dedupePowerSlots() {
    size_t removed = 0;
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (!powerSlots_[i].valid) {
        continue;
      }
      for (size_t j = i + 1; j < MAX_POWER_SLOTS; ++j) {
        if (!powerSlots_[j].valid || powerSlots_[j].nodeId != powerSlots_[i].nodeId) {
          continue;
        }
        PowerReport merged = powerSlots_[i];
        if (shouldPreferPowerSlot(powerSlots_[i], powerSlots_[j])) {
          merged = powerSlots_[j];
        }
        mergePowerSlotMetadata(&merged, powerSlots_[i]);
        mergePowerSlotMetadata(&merged, powerSlots_[j]);
        powerSlots_[i] = merged;
        memset(&powerSlots_[j], 0, sizeof(powerSlots_[j]));
        ++removed;
      }
    }
    if (removed > 0) {
      statusDirty_ = true;
      legacyStateDirty_ = true;
      invalidateLegacyDiscovery();
      addEvent("deduped %u duplicate power slots", (unsigned)removed);
    }
  }

  NodeMapEntry* findOrCreateNodeMap(uint16_t nodeId) {
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid && nodeMap_[i].nodeId == nodeId) {
        return &nodeMap_[i];
      }
    }
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (!nodeMap_[i].valid) {
        return &nodeMap_[i];
      }
    }
    return nullptr;
  }

  PowerReport* findOrCreatePowerSlot(uint16_t nodeId, uint16_t shortAddr) {
    (void)shortAddr;
    nodeId = normalizeNodeId(nodeId);
    PowerReport* freeSlot = nullptr;
    PowerReport* chosen = nullptr;
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (!powerSlots_[i].valid) {
        if (freeSlot == nullptr) {
          freeSlot = &powerSlots_[i];
        }
        continue;
      }
      if (powerSlots_[i].nodeId != nodeId) {
        continue;
      }
      if (chosen == nullptr) {
        chosen = &powerSlots_[i];
        continue;
      }
      // Older builds could accumulate multiple slots for the same node because
      // the short address was treated as part of the identity. MQTT topics are
      // node-based, so duplicates lead to multiple conflicting publishes.
      powerSlots_[i].valid = false;
      statusDirty_ = true;
    }
    if (chosen != nullptr) {
      return chosen;
    }
    return freeSlot;
  }

  const char* lookupLongAddrForNode(uint16_t nodeId) const {
    nodeId = normalizeNodeId(nodeId);
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid && nodeMap_[i].nodeId == nodeId) {
        return nodeMap_[i].longAddr;
      }
    }
    return nullptr;
  }

  const PowerReport* findPowerSlotByNodeId(uint16_t nodeId) const {
    nodeId = normalizeNodeId(nodeId);
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (powerSlots_[i].valid && powerSlots_[i].nodeId == nodeId) {
        return &powerSlots_[i];
      }
    }
    return nullptr;
  }

  const char* lookupPanelLabel(const char* longAddr) const {
    if (longAddr == nullptr || longAddr[0] == '\0') {
      return nullptr;
    }
    for (uint16_t i = 0; i < panelFieldCount_; ++i) {
      if (panelMap_[i].longAddr[0] != '\0' && strcmp(panelMap_[i].longAddr, longAddr) == 0) {
        return panelMap_[i].label;
      }
    }
    return nullptr;
  }

  void refreshPanelLabelsFromNodeMap() {
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (!powerSlots_[i].valid) continue;
      const char* label = lookupPanelLabel(powerSlots_[i].longAddr);
      copyString(powerSlots_[i].panelLabel, sizeof(powerSlots_[i].panelLabel), label ? label : "");
    }
  }

  uint16_t countValidNodeMap() const {
    uint16_t count = 0;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid) {
        ++count;
      }
    }
    return count;
  }

  uint16_t countConfirmedNodeMap() const {
    uint16_t count = 0;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid && !nodeMap_[i].pending) {
        ++count;
      }
    }
    return count;
  }

  bool nodeTableMatchesConfiguredPanels(bool requirePending) const {
    const uint16_t expectedCount = countPanelMapLongAddrs();
    if (expectedCount == 0 || countValidNodeMap() != expectedCount) {
      return false;
    }
    uint16_t panelIndex = 0;
    for (size_t i = 0; i < TIGO_MAX_OPTIMIZERS; ++i) {
      if (panelMap_[i].longAddr[0] == '\0') {
        continue;
      }
      const uint16_t nodeId = (uint16_t)(TIGO_NODE_ID_BASE + panelIndex);
      const uint16_t expectedRaw = requirePending ? (uint16_t)(nodeId | 0x8000U) : nodeId;
      bool found = false;
      for (size_t j = 0; j < MAX_NODE_MAP; ++j) {
        if (nodeMap_[j].valid && nodeMap_[j].rawNodeId == expectedRaw &&
            strcmp(nodeMap_[j].longAddr, panelMap_[i].longAddr) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
      ++panelIndex;
    }
    return panelIndex == expectedCount;
  }

  uint16_t countPendingNodeMap() const {
    uint16_t count = 0;
    for (size_t i = 0; i < MAX_NODE_MAP; ++i) {
      if (nodeMap_[i].valid && nodeMap_[i].pending) {
        ++count;
      }
    }
    return count;
  }

  uint16_t countValidPower() const {
    uint16_t count = 0;
    for (size_t i = 0; i < MAX_POWER_SLOTS; ++i) {
      if (powerSlots_[i].valid) {
        ++count;
      }
    }
    return count;
  }

  void bytesToHex(const uint8_t* data, size_t len, char* out, size_t outLen) const {
    if (outLen == 0) {
      return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len && (pos + 2) < outLen; ++i) {
      pos += (size_t)snprintf(out + pos, outLen - pos, "%02X", data[i]);
    }
    out[(pos < outLen) ? pos : (outLen - 1)] = '\0';
  }

  const char* frameTypeName(uint16_t typeCode) const {
    switch (typeCode) {
      case 0x0014: return "enum_start";
      case 0x0015: return "enum_start_ack";
      case 0x0038: return "enum_info_req";
      case 0x0039: return "enum_info";
      case 0x003A: return "network_info_req";
      case 0x003B: return "network_info";
      case 0x003C: return "assign_gateway";
      case 0x003D: return "assign_gateway_ack";
      case 0x000A: return "version_req";
      case 0x000B: return "version";
      case 0x0E02: return "e02_probe";
      case 0x0006: return "e02_ack";
      case 0x000E: return "status_req";
      case 0x000F: return "status";
      case 0x0052: return "tap_test_probe";
      case 0x0053: return "tap_test_probe_ack";
      case 0x0148: return "rx_poll";
      case 0x0149: return "rx_response";
      case 0x0B00: return "rsd_control";
      case 0x0B01: return "rsd_control_ack";
      case 0x0B0F: return "pv_subcmd";
      case 0x0B10: return "pv_subcmd_ack";
      default: return "unknown";
    }
  }

  void buildLastFrameLiveLine(uint32_t now, char* out, size_t outLen) const {
    if (outLen == 0) {
      return;
    }
    if (!lastFrameValid_) {
      copyString(out, outLen, "no TAP frame received yet");
      return;
    }
    char age[20];
    formatUnsigned32(elapsed(now, lastFrameMs_), age, sizeof(age));
    snprintf(out, outLen,
             "#%lu age=%sms dir=%s addr=%04X gw=%04X type=%04X(%s) crc=%s len=%u payload=%s%s",
             (unsigned long)framesRx_,
             age,
             lastFrameFromGateway_ ? "gateway" : "tap",
             lastFrameAddrRaw_,
             lastFrameGatewayId_,
             lastFrameTypeCode_,
             frameTypeName(lastFrameTypeCode_),
             lastFrameCrcOk_ ? "ok" : "bad",
             (unsigned)lastFramePayloadLen_,
             lastFramePayloadPreviewHex_[0] ? lastFramePayloadPreviewHex_ : "-",
             lastFramePayloadTruncated_ ? "..." : "");
    out[outLen - 1] = '\0';
  }

  // --------------------------
  // Data members
  // --------------------------
  GatewayStreamParser parser_;
  WebServer webServer_{80};
  Esp32c6PlatformRuntime platformRuntime_;
  WiFiClient wifiClient_;
  PubSubClient mqttClient_;
  bool persistentStoreReady_ = false;
  bool mdnsStarted_ = false;

  uint16_t gatewayId_;
  uint16_t nextPacketNumber_;
  uint16_t lastRequestedPacketNumber_;
  uint16_t persistedGatewayId_ = 0;
  uint16_t persistedPacketCursor_ = 0;
  uint16_t confirmedPacketCursor_ = 0;
  BootCursorStrategy bootCursorStrategy_ = BootCursorStrategy::Persisted;
  BootOptionsSettings bootOptions_{};
  BootJournalSettings bootJournal_{};
  bool bootJournalLoaded_ = false;
  int8_t activeMutationIndex_ = -1;
  bool bootJournalDirty_ = false;
  uint32_t bootJournalDirtySinceMs_ = 0;
  uint32_t lastCursorCheckpointMs_ = 0;
  bool cursorConfirmedThisBoot_ = false;
  uint32_t cursorForwardResyncCount_ = 0;
  uint32_t cursorForwardDistance_ = 0;
  uint32_t cursorDuplicateResponseCount_ = 0;
  uint32_t cursorMalformedPayloadCount_ = 0;
  uint32_t cursorPersistenceHoldCount_ = 0;
  uint32_t cursorCheckpointFailureCount_ = 0;
  bool cursorEpochResetPending_ = false;
  bool cursorStallRecoveryAttempted_ = false;
  bool targetedCursorRecoveryActive_ = false;
  uint32_t cursorRecoveryPollTimeoutBaseline_ = 0;
  uint32_t bootStartedMs_ = 0;
  uint32_t firstReceiveResponseMs_ = 0;
  uint32_t firstPowerFrameMs_ = 0;
  uint32_t firstReleasedPowerEvidenceMs_ = 0;
  uint32_t lastReleasedPowerEvidenceMs_ = 0;
  uint32_t releasedPowerEvidenceCount_ = 0;
  uint32_t destructiveManagementFramesTx_ = 0;
  uint32_t activeRfFramesTx_ = 0;
  uint32_t gatewayFrameTxGeneration_ = 0;
  uint32_t lastGatewayFrameTxMs_ = 0;
  char gatewayLongAddr_[17];
  bool currentRadioDescriptorValid_ = false;
  uint8_t currentRadioDescriptor_[TIGO_RADIO_DESCRIPTOR_LEN];
  char currentRadioDescriptorTapLongAddr_[17];
  uint32_t currentRadioDescriptorUpdatedMs_ = 0;
  bool workingRadioDescriptorValid_ = false;
  uint8_t workingRadioDescriptor_[TIGO_RADIO_DESCRIPTOR_LEN];
  char workingRadioDescriptorTapLongAddr_[17];
  bool rollbackRadioDescriptorValid_ = false;
  uint8_t rollbackRadioDescriptor_[TIGO_RADIO_DESCRIPTOR_LEN];
  char rollbackRadioDescriptorTapLongAddr_[17];
  bool radioJoinSeedValid_ = false;
  uint8_t radioJoinSeed_[TIGO_JOIN_SEED_LEN];
  uint32_t radioJoinSeedProfileFingerprint_ = 0;
  bool forceLearnAfterProfileWrite_ = false;

  bool awaitingReceiveResponse_;
  uint32_t lastPollSentMs_;
  uint32_t lastPeriodicPingMs_;
  uint32_t lastPeriodicNodeTableMs_;
  uint32_t lastPeriodicNetworkStatusMs_;
  uint32_t lastPollTimeoutEventMs_;
  uint32_t lastStateSaveMs_;

  uint32_t framesRx_;
  uint32_t framesCrcError_;
  uint32_t lastTapResponseMs_;
  uint32_t tapResponsesRx_;
  uint32_t pollsSent_;
  uint32_t pollTimeouts_;
  bool activePollingEnabled_;
  bool persistentStateDirty_ = false;
  uint32_t persistentStateDirtySinceMs_ = 0;

  bool wifiConnected_;
  bool apMode_;
  wl_status_t lastWifiStatus_;
  bool lastFrameValid_ = false;
  bool lastFrameCrcOk_ = false;
  bool lastFrameFromGateway_ = false;
  uint16_t lastFrameGatewayId_ = 0;
  uint16_t lastFrameAddrRaw_ = 0;
  uint16_t lastFrameTypeCode_ = 0;
  uint16_t lastFramePayloadLen_ = 0;
  char lastFramePayloadPreviewHex_[129] = "";
  bool lastFramePayloadTruncated_ = false;
  uint32_t lastFrameMs_ = 0;

  NodeMapEntry nodeMap_[MAX_NODE_MAP];
  PowerReport powerSlots_[MAX_POWER_SLOTS];
  RecentEvent events_[MAX_EVENTS];
  uint8_t recentEventHead_;
  InterestingFrame interestingFrames_[MAX_INTERESTING_FRAMES];
  uint32_t interestingFrameSeq_;
  uint8_t interestingFrameHead_;
  uint32_t interestingFrameDropped_;
  uint16_t lastInterestingGatewayId_;

  uint8_t sequence_ = 0;
  TapCommandState commandState_;
  uint16_t lastCommandResponseType_ = 0;
  uint16_t lastCommandResponseGatewayId_ = 0;
  uint8_t lastCommandResponseDsn_ = 0;
  bool lastCommandOk_ = false;
  uint32_t lastCommandCompletedMs_ = 0;
  uint32_t lastCommandCompletionGeneration_ = 0;
  PassivePvExpectation passivePvExpectations_[MAX_PASSIVE_PV_EXPECTATIONS];
  bool bootEnumeratePending_ = false;
  bool bootVersionPending_ = false;
  bool bootRadioConfigPending_ = false;
  bool bootGatewaySelector0Pending_ = false;
  bool bootNetworkStatusPending_ = false;
  bool bootNodeTablePending_ = false;
  bool bootGatewaySelector1Pending_ = false;
  bool bootNodeTableEndPending_ = false;
  bool bootEnumerateTried_ = false;
  uint8_t bootCcaStep_ = UINT8_MAX;
  uint8_t bootCcaWaitingStep_ = UINT8_MAX;
  uint8_t bootCcaRetryCount_ = 0;
  uint32_t bootCcaNextActionMs_ = 0;
  bool bootCcaWarmAttachTried_ = false;
  bool bootCcaWarmAttachPending_ = false;
  TapBootPath tapBootPath_ = TapBootPath::Unknown;
  WarmAttachPhase warmAttachPhase_ = WarmAttachPhase::Idle;
  uint16_t warmAttachCandidates_[8]{};
  uint8_t warmAttachCandidateCount_ = 0;
  uint8_t warmAttachCandidateIndex_ = 0;
  uint16_t warmAttachProbeId_ = 0;
  uint16_t warmAttachRecoveryTargetId_ = 0;
  uint32_t warmAttachNextActionMs_ = 0;
  bool warmAttachCommandPending_ = false;
  bool warmAttachRecoverySearch_ = false;
  bool factoryAddressAssignmentAttempted_ = false;
  bool readOnlyWarmAttachProtectsState_ = false;
  bool recoveryAuthorized_ = false;
  bool lastRsdControlKnown_ = false;
  bool lastRsdRunState_ = false;
  uint32_t lastRsdControlAckMs_ = 0;
  bool replayExclusiveMode_ = false;
  TraceReplayStep traceReplaySteps_[TIGO_TRACE_REPLAY_MAX_STEPS]{};
  TraceReplayResult traceReplayResults_[TIGO_TRACE_REPLAY_MAX_STEPS]{};
  TraceReplayState traceReplayState_ = TraceReplayState::Idle;
  uint8_t traceReplayStepCount_ = 0;
  uint8_t traceReplayStepIndex_ = 0;
  uint32_t traceReplayStartedMs_ = 0;
  uint32_t traceReplayHoldUntilOffsetMs_ = 0;
  uint32_t traceReplayMaxLateMs_ = 250;
  uint32_t traceReplayPollIntervalMs_ = 15;
  uint32_t traceReplayPollGuardMs_ = 8;
  uint32_t traceReplayPollsAtStart_ = 0;
  uint32_t traceReplayPollsAtEnd_ = 0;
  uint32_t traceReplayWaitingGeneration_ = 0;
  uint32_t traceReplayCommandDeadlineMs_ = 0;
  uint32_t traceReplayNoResponseDeadlineMs_ = 0;
  uint16_t traceReplayExpectedGatewayId_ = 0;
  uint16_t traceReplayNoResponseType_ = 0;
  uint16_t traceReplayNoResponseGatewayId_ = 0;
  uint8_t traceReplayExpectedDsn_ = 0;
  bool traceReplayReceivePumpEnabled_ = false;
  bool traceReplayUnexpectedResponse_ = false;
  bool traceReplayIdentityMismatch_ = false;
  bool traceReplayAllowStateChanging_ = false;
  bool traceReplayAllowActiveRf_ = false;
  uint16_t traceReplayBaselineGatewayId_ = 0;
  uint32_t traceReplayBaselineNodeTableHash_ = 0;
  uint32_t traceReplayBaselineRadioFingerprint_ = 0;
  uint8_t traceReplayBaselineNetworkMode_ = 0;
  uint16_t traceReplayBaselineNetworkConfirmed_ = 0;
  uint16_t traceReplayBaselineNetworkExpected_ = 0;
  uint32_t traceReplayBaselineStateChangingTx_ = 0;
  uint32_t traceReplayBaselineActiveRfTx_ = 0;
  uint32_t traceReplayBaselineCursorMalformed_ = 0;
  uint32_t traceReplayBaselineCursorDuplicate_ = 0;
  uint32_t traceReplayBaselineCursorForwardResync_ = 0;
  char traceReplayExpectedTapEui_[17] = "";
  char traceReplayFailureReason_[128] = "";

  RawFrameCaptureItem rawFrameCaptureQueue_[TIGO_RAW_FRAME_CAPTURE_QUEUE_LEN]{};
  size_t rawFrameCaptureHead_ = 0;
  size_t rawFrameCaptureCount_ = 0;
  uint32_t rawFrameCaptureDropped_ = 0;

  char webVersionText_[96];
  UserPanelMapEntry panelMap_[TIGO_MAX_OPTIMIZERS];
  uint16_t panelFieldCount_;
  MqttRuntimeSettings mqttSettings_;
  uint8_t mqttMigrationFlags_;
  char mqttClientId_[48];
  bool mqttConnected_;
  uint32_t lastMqttConnectAttemptMs_;
  uint32_t lastMqttHeartbeatPublishMs_;
  uint32_t lastMqttStatusPublishMs_;
  uint32_t lastMqttTelemetryPublishMs_;
  uint32_t lastLegacyDiscoveryStepMs_;
  uint32_t lastLegacyStateClearMs_;
  uint32_t lastInvalidNodeStatusClearMs_;
  uint32_t lastDeprecatedPanelTelemetryClearMs_;
  uint32_t lastDeprecatedSystemStatusClearMs_;
  uint32_t lastLegacyDiscoveryFailureLogMs_;
  uint32_t lastSerialWifiStatusMs_;
  uint32_t lastSerialPowerStatusMs_;
  uint32_t lastSerialRxPollStatusMs_;
  uint32_t lastWebRequestMs_;
  uint32_t lastWebServerRecoverMs_;
  String* jsonBuildTarget_;
  bool otaInProgress_;
  bool pollingEnabledBeforeOta_;
  bool nodeWakeActive_;
  bool nodeWakeCompleted_;
  bool nodeWakeLearnActive_;
  bool nodeWakeLearnWaitCountdown_;
  bool nodeWakeSkipLearn_;
  bool nodeWakeLearnRestartAfterPvRun_;
  uint32_t forceLearnUntilMs_;
  NodeSeedState nodeSeedState_;
  bool nodeSeedAwaitingCommand_;
  bool nodeWakeConfigFallbackActive_;
  bool nodeWakeForcePvConfig_;
  uint16_t nodeWakeStep_;
  uint32_t nodeWakeNextActionMs_;
  uint32_t nodeWakeLearnStartedMs_;
  uint32_t nodeWakeLearnLastNetworkStatusMs_;
  uint32_t nodeWakeLearnLastJoinSeedMs_;
  uint16_t nodeWakeLearnWarmupStep_;
  RfNodePromotionState rfNodePromotionState_;
  uint16_t rfNodePromotionNodeId_;
  char rfNodePromotionLongAddr_[17];
  uint16_t nodeSeedNextPanelIndex_;
  uint16_t nodeSeedPanelCount_;
  uint16_t nodeSeedChunksTotal_;
  uint16_t nodeSeedChunksAcked_;
  uint8_t nodeSeedRetryCount_;
  uint16_t nodeSeedVerifyStart_;
  uint16_t nodeSeedVerifiedReadbackEntries_;
  uint16_t lastNodeTableStart_;
  uint16_t lastNodeTableEntryCount_;
  uint32_t lastNodeTableMs_;
  uint32_t lastNodeWakeAttemptMs_;
  bool legacyDiscoveryPublished_;
  uint16_t legacyDiscoveryClearIndex_;
  uint16_t invalidNodeStatusClearIndex_;
  uint16_t deprecatedPanelTelemetryClearIndex_;
  uint16_t deprecatedSystemStatusClearIndex_;
  uint16_t legacyDiscoveryPublishSlotIndex_;
  bool legacyStateTopicsCleared_;
  bool invalidNodeStatusTopicsCleared_;
  bool deprecatedPanelTelemetryTopicsCleared_;
  bool deprecatedSystemStatusTopicsCleared_;
  bool legacyStateDirty_;
  bool statusDirty_;
  char lastResetReason_[48];
  char lastNodeTableAckHex_[64];
  char lastNetworkStatusAckHex_[64];
  char lastRadioConfigAckHex_[64];
  bool lastNetworkStatusValid_ = false;
  uint32_t lastNetworkStatusMs_ = 0;
  uint8_t lastNetworkMode_ = 0;
  uint16_t lastNetworkCountdown_ = 0;
  uint16_t lastNetworkFlags_ = 0;
  uint16_t lastNetworkConfirmedNodes_ = 0;
  uint16_t lastNetworkExpectedNodes_ = 0;
  uint8_t lastPvSubcommand_ = 0;
  size_t lastPvSubcommandRequestLen_ = 0;
  bool lastPvSubcommandRequestTruncated_ = false;
  char lastPvSubcommandRequestHex_[160];
  char lastPvSubcommandAckHex_[192];
  uint8_t lastPvAckStatusFlags_ = 0;
  uint8_t lastPvAckResponseSubcmd_ = 0;
  char lastPvAckBodyHex_[192];
  char lastSeedError_[96];
  char lastCommandName_[24];
  char lastCommandMessage_[96];
  bool rebootRequested_ = false;
  uint32_t rebootAtMs_ = 0;
};

TigoTapMaster g_tigo;

void opentaptoxEsp32c6Setup() {
  Serial.begin(TIGO_USB_BAUD);
  delay(300);
  Serial.printf("\r\nOTX boot version=%s usb_baud=%lu rs485_baud=%lu rx=%d tx=%d\r\n",
                TIGO_FIRMWARE_VERSION,
                (unsigned long)TIGO_USB_BAUD,
                (unsigned long)TIGO_RS485_BAUD,
                TIGO_RS485_RX_PIN,
                TIGO_RS485_TX_PIN);
  g_tigo.begin();
}

void opentaptoxEsp32c6Loop() {
  g_tigo.loop();
}

static void handleTigoMqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  if (g_tigoMqttCallbackTarget != nullptr) {
    g_tigoMqttCallbackTarget->dispatchMqttMessage(topic, payload, length);
  }
}
