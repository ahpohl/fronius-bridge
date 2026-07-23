#ifndef EASY_METER_H_
#define EASY_METER_H_

#include "change_gate.h"
#include "config_yaml.h"
#include "meter_master.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <chrono>
#include <condition_variable>
#include <expected>
#include <fronius/fronius.h>
#include <memory>
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
// It reuses fronius::ModbusError for its error/severity model: framing errors
// map to EPROTO -> TRANSIENT, so the loop reconnects, re-flushes the serial
// buffers via tryConnect()'s tcflush, and re-syncs the telegram stream.
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
  // Block for the given backoff period, or return early on shutdown. Pure
  // wait: the caller grows the delay for the next attempt (see runLoop).
  void sleepBackoff(std::chrono::seconds duration);
  std::expected<void, ModbusError> tryConnect(void);
  std::expected<void, ModbusError> readTelegram(void);
  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateDeviceAndJson(void);

  // Held by value: AppConfig's std::vector<MeterConfig> may reallocate. cfg_
  // is the kind-agnostic envelope (name, slave); ecfg_ is the EBZ-specific
  // body (rtu line + grid assumptions) extracted from cfg_.body.
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
  // The callbacks and the mutex guarding them (cbMutex_) live in the
  // MeterMaster base; runLoop reads and fires them under that mutex.
  SignalHandler &handler_;
  std::condition_variable cv_;
  std::thread worker_;

  // The EBZ re-parses its identity from every SML telegram, so emit the device
  // callback only when that identity changes; otherwise it would be
  // republished once per telegram. Touched only from the poll thread.
  ChangeGate<MeterTypes::Device> deviceGate_;
};

#endif /* EASY_METER_H_ */
