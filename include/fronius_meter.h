#ifndef FRONIUS_METER_H_
#define FRONIUS_METER_H_

#include "change_gate.h"
#include "config_yaml.h"
#include "meter_master.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <expected>
#include <fronius/fronius.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <thread>

class FroniusMeter : public MeterMaster {
public:
  explicit FroniusMeter(const MeterConfig &cfg, SignalHandler &signalHandler,
                        std::shared_ptr<FroniusBus> bus);
  ~FroniusMeter() override;

  // Non-copyable, non-movable — owns a thread. Also deleted in the base;
  // restated here for clarity at the concrete type.
  FroniusMeter(const FroniusMeter &) = delete;
  FroniusMeter &operator=(const FroniusMeter &) = delete;
  FroniusMeter(FroniusMeter &&) = delete;
  FroniusMeter &operator=(FroniusMeter &&) = delete;

  std::string getJsonDump(void) const;
  MeterTypes::Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  // Returns true when the identity was read on this call and differs from what
  // was last emitted, so runLoop should publish it. Read over Modbus, so the
  // identity is read once per connection and skipped thereafter.
  std::expected<bool, ModbusError> updateDeviceAndJson(void);

private:
  static ModbusDeviceConfig makeDeviceConfig(const FroniusMeterConfig &cfg);
  void runLoop();

  std::shared_ptr<FroniusBus> bus_;
  std::shared_ptr<Meter> meter_;
  // Held by value: AppConfig's std::vector<MeterConfig> may reallocate. cfg_
  // is the kind-agnostic envelope (name, slave); fcfg_ is the Fronius-specific
  // body extracted from cfg_.body at construction.
  const MeterConfig cfg_;
  const FroniusMeterConfig fcfg_;
  std::shared_ptr<spdlog::logger> logger_;

  // Bus-level callback IDs registered by this master. The destructor removes
  // them before tearing down the state their lambdas capture, since the bus
  // thread may outlive any individual master on a shared bus.
  std::vector<FroniusBus::CallbackId> busCallbackIds_;

  // --- values and device info ---
  MeterTypes::Device device_;
  MeterTypes::Values values_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::json jsonDevice_;

  // --- threading ---
  // The callbacks and the mutex guarding them (cbMutex_) live in the
  // MeterMaster base; runLoop and the bus/device callbacks read them there.
  SignalHandler &handler_;
  std::thread worker_;
  std::atomic<bool> connected_{false};
  std::condition_variable cv_;

  // Emits the device callback only when the identity actually changes. Read
  // once per connection (hasValue() guards the re-read), so the gate records
  // that single value.
  ChangeGate<MeterTypes::Device> deviceGate_;

  // Armed by the bus-connect callback, consumed by runLoop, which then clears
  // deviceGate_ so the next poll re-reads the identity. The indirection keeps
  // the gate single-threaded: libfronius fires the callback on the bus thread,
  // not on the poll thread that owns the gate.
  std::atomic<bool> deviceStale_{false};
};

#endif /* FRONIUS_METER_H_ */
