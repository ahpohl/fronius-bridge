#ifndef INVERTER_MASTER_H_
#define INVERTER_MASTER_H_

#include "config_yaml.h"
#include "inverter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <expected>
#include <fronius/fronius_bus.h>
#include <fronius/inverter.h>
#include <fronius/modbus_config.h>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
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
  std::expected<void, ModbusError> updateEventsAndJson(void);
  // Returns true if the device info was (re)read from the wire on this
  // call, false if it was already cached from a previous call and nothing
  // changed. Used by runLoop to skip publishing the (unchanged) device
  // JSON to MQTT on every poll cycle.
  std::expected<bool, ModbusError> updateDeviceAndJson(void);

  void
  setValueCallback(std::function<void(std::string, InverterTypes::Values)> cb);
  void
  setEventCallback(std::function<void(std::string, InverterTypes::Events)> cb);
  void
  setDeviceCallback(std::function<void(std::string, InverterTypes::Device)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

  static ModbusBusConfig makeBusConfig(const InverterConfig &cfg);

private:
  static ModbusDeviceConfig makeDeviceConfig(const InverterConfig &cfg);
  void runLoop();

  std::shared_ptr<FroniusBus> bus_;
  std::shared_ptr<Inverter> inverter_;
  // Held by value — with std::vector<InverterConfig> in AppConfig, a
  // reference would dangle on vector reallocation. The config is small
  // and copyable.
  const InverterConfig cfg_;
  std::shared_ptr<spdlog::logger> logger_;

  // IDs of bus-level callbacks this master has registered. Removed in
  // the destructor before any state captured by those callbacks (this,
  // modbusLogger_, handler_) is torn down. With a shared bus, the bus
  // thread can outlive any individual master; leaving the callbacks
  // attached would be a use-after-free hazard the next time the bus
  // fired one.
  std::vector<FroniusBus::CallbackId> busCallbackIds_;

  // --- values and events ---
  InverterTypes::Device device_;
  InverterTypes::Values values_;
  InverterTypes::Events events_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::ordered_json jsonEvents_;
  nlohmann::json jsonDevice_;
  std::optional<std::size_t> lastEventsHash_;

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
  std::atomic<bool> deviceUpdated_{false};
};

#endif /* INVERTER_MASTER_H_ */
