#ifndef EASY_METER_H_
#define EASY_METER_H_

#include "config_yaml.h"
#include "meter_master.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <expected>
#include <fronius/modbus_error.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// EasyMeter — wire-side reader for an EBZ Easymeter.
//
// Unlike FroniusMeter this is NOT a Modbus device:
//   - It owns a serial line exclusively (flock + TIOCEXCL); the line cannot
//     be shared, so the EBZ never joins the shared FroniusBus registry.
//   - It is event-driven: the worker blocks reading inbound SML/OBIS
//     telegrams and publishes as they arrive. There is no poll interval.
//   - The EBZ reports only active power and energy; reactive/apparent power
//     and energy and per-phase currents are derived from the assumed grid
//     parameters in EasyMeterConfig::grid.
//
// It reuses fronius::ModbusError for its error/severity model (telegram
// framing errors map to EPROTO -> TRANSIENT, so the loop simply reconnects,
// re-flushes the serial buffers via tryConnect()'s tcflush, and re-syncs the
// telegram stream from scratch). Callback storage and cbMutex_ are inherited
// from MeterMaster; this class fires the inherited callbacks from its worker.
// ---------------------------------------------------------------------------

class EasyMeter : public MeterMaster {
public:
  explicit EasyMeter(const MeterConfig &cfg, SignalHandler &signalHandler);
  ~EasyMeter() override;

  // Non-copyable, non-movable — owns a thread and a file descriptor.
  EasyMeter(const EasyMeter &) = delete;
  EasyMeter &operator=(const EasyMeter &) = delete;
  EasyMeter(EasyMeter &&) = delete;
  EasyMeter &operator=(EasyMeter &&) = delete;

  static constexpr size_t BUFFER_SIZE = 64;
  static constexpr size_t TELEGRAM_SIZE = 368;

private:
  void runLoop();
  MeterTypes::ErrorAction
  handleResult(std::expected<void, ModbusError> &&result);
  void disconnect(void);
  std::expected<void, ModbusError> tryConnect(void);
  std::expected<void, ModbusError> readTelegram(void);
  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateDeviceAndJson(void);

  // Held by value: AppConfig's std::vector<MeterConfig> may reallocate.
  // cfg_ carries the kind-agnostic envelope (name, slave); ecfg_ is the
  // EBZ-specific body (rtu line + grid assumptions) extracted from cfg_.body
  // at construction.
  const MeterConfig cfg_;
  const EasyMeterConfig ecfg_;

  MeterTypes::Values values_;
  MeterTypes::Device device_;
  std::string telegram_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::json jsonDevice_;
  std::shared_ptr<spdlog::logger> logger_;
  int serialPort_{-1};

  // --- threading ---
  // The value/device/availability callbacks and the mutex guarding them
  // (cbMutex_) live in the MeterMaster base; this master reads/fires them
  // under that mutex from runLoop.
  SignalHandler &handler_;
  std::condition_variable cv_;
  std::thread worker_;
};

#endif /* EASY_METER_H_ */
