#ifndef INVERTER_MASTER_H_
#define INVERTER_MASTER_H_

#include "change_gate.h"
#include "config_yaml.h"
#include "inverter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <expected>
#include <fronius/fronius.h>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <thread>

class InverterMaster {
public:
  explicit InverterMaster(const InverterConfig &cfg,
                          SignalHandler &signalHandler,
                          std::shared_ptr<FroniusBus> bus);
  virtual ~InverterMaster();

  // Non-copyable, non-movable — owns a thread.
  InverterMaster(const InverterMaster &) = delete;
  InverterMaster &operator=(const InverterMaster &) = delete;
  InverterMaster(InverterMaster &&) = delete;
  InverterMaster &operator=(InverterMaster &&) = delete;

  const std::string &name() const { return cfg_.name; }

  std::string getJsonDump(void) const;
  InverterTypes::Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<bool, ModbusError> updateEventsAndJson(void);
  // Returns true if the device identity was read on this call and differs
  // from what was last emitted (so runLoop should publish it), false if the
  // identity was already read and is unchanged. The inverter is read over
  // Modbus, so identity is read once and skipped thereafter.
  std::expected<bool, ModbusError> updateDeviceAndJson(void);

  void
  setValueCallback(std::function<void(std::string, InverterTypes::Values)> cb);
  void
  setEventCallback(std::function<void(std::string, InverterTypes::Events)> cb);
  void
  setDeviceCallback(std::function<void(std::string, InverterTypes::Device)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

private:
  static ModbusDeviceConfig makeDeviceConfig(const InverterConfig &cfg);
  void runLoop();

  // Publish an availability state ("connected"/"disconnected")
  // through the gate, so each distinct state is emitted once on transition.
  // Thread-safe; called from the bus callbacks and runLoop.
  void publishAvailability(std::string state);

  std::shared_ptr<FroniusBus> bus_;
  std::shared_ptr<Inverter> inverter_;
  // Held by value: AppConfig's std::vector<InverterConfig> may reallocate.
  const InverterConfig cfg_;
  std::shared_ptr<spdlog::logger> logger_;

  // Bus-level callback IDs registered by this master. The destructor
  // removes them before tearing down state captured by their lambdas
  // (this, logger_, handler_), since the bus thread may outlive any
  // individual master on a shared bus.
  std::vector<FroniusBus::CallbackId> busCallbackIds_;

  // --- values and events ---
  InverterTypes::Device device_;
  InverterTypes::Values values_;
  InverterTypes::Events events_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::ordered_json jsonEvents_;
  nlohmann::json jsonDevice_;

  // --- threading / callbacks ---
  std::function<void(std::string, InverterTypes::Values)> valueCallback_;
  std::function<void(std::string, InverterTypes::Events)> eventCallback_;
  std::function<void(std::string, InverterTypes::Device)> deviceCallback_;
  std::function<void(std::string)> availabilityCallback_;
  SignalHandler &handler_;
  mutable std::mutex cbMutex_;
  std::thread worker_;
  std::atomic<bool> connected_{false};
  std::condition_variable cv_;

  // --- change gates
  ChangeGate<InverterTypes::Events> eventsGate_;
  ChangeGate<InverterTypes::Device> deviceGate_;
  ChangeGate<std::string> availabilityGate_;
};

#endif /* INVERTER_MASTER_H_ */
