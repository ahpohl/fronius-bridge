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
#include <utility>

class InverterMaster {
public:
  explicit InverterMaster(const InverterConfig &cfg,
                          SignalHandler &signalHandler,
                          std::shared_ptr<FroniusBus> bus);
  virtual ~InverterMaster();

  std::string getJsonDump(void) const;
  InverterTypes::Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateEventsAndJson(void);
  std::expected<void, ModbusError> updateDeviceAndJson(void);

  void setValueCallback(std::function<void(std::string)> cb);
  void setEventCallback(std::function<void(std::string)> cb);
  void setDeviceCallback(std::function<void(std::string)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

  static ModbusBusConfig makeBusConfig(const InverterConfig &cfg);

private:
  static ModbusDeviceConfig makeDeviceConfig(const InverterConfig &cfg);
  void runLoop();

  std::shared_ptr<FroniusBus> bus_;
  std::shared_ptr<Inverter> inverter_;
  const InverterConfig &cfg_;
  std::shared_ptr<spdlog::logger> modbusLogger_;

  // --- values and events ---
  InverterTypes::Device device_;
  InverterTypes::Values values_;
  InverterTypes::Events events_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::ordered_json jsonEvents_;
  nlohmann::json jsonDevice_;
  std::optional<std::size_t> lastEventsHash_;

  // --- threading / callbacks ---
  std::function<void(std::string)> valueCallback_;
  std::function<void(std::string)> eventCallback_;
  std::function<void(std::string)> deviceCallback_;
  std::function<void(std::string)> availabilityCallback_;
  SignalHandler &handler_;
  mutable std::mutex cbMutex_;
  std::thread worker_;
  std::atomic<bool> connected_{false};
  std::condition_variable cv_;
  std::atomic<bool> deviceUpdated_{false};
};

#endif /* INVERTER_MASTER_H_ */
