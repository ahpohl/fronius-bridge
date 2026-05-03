#ifndef METER_MASTER_H_
#define METER_MASTER_H_

#include "config_yaml.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <expected>
#include <fronius/fronius_bus.h>
#include <fronius/meter.h>
#include <fronius/modbus_config.h>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <thread>
#include <utility>

class MeterMaster {
public:
  explicit MeterMaster(const MeterMasterConfig &cfg,
                       SignalHandler &signalHandler,
                       std::shared_ptr<FroniusBus> bus);
  virtual ~MeterMaster();

  std::string getJsonDump(void) const;
  MeterTypes::Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateDeviceAndJson(void);

  void
  setValueCallback(std::function<void(std::string, MeterTypes::Values)> cb);
  void
  setDeviceCallback(std::function<void(std::string, MeterTypes::Device)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

  static ModbusBusConfig makeBusConfig(const MeterMasterConfig &cfg);

private:
  static ModbusDeviceConfig makeDeviceConfig(const MeterMasterConfig &cfg);
  void runLoop();

  std::shared_ptr<FroniusBus> bus_;
  std::shared_ptr<Meter> meter_;
  const MeterMasterConfig &cfg_;
  std::shared_ptr<spdlog::logger> modbusLogger_;

  // --- values and device info ---
  MeterTypes::Device device_;
  MeterTypes::Values values_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::json jsonDevice_;

  // --- threading / callbacks ---
  std::function<void(std::string, MeterTypes::Values)> valueCallback_;
  std::function<void(std::string, MeterTypes::Device)> deviceCallback_;
  std::function<void(std::string)> availabilityCallback_;
  SignalHandler &handler_;
  mutable std::mutex cbMutex_;
  std::thread worker_;
  std::atomic<bool> connected_{false};
  std::condition_variable cv_;
  std::atomic<bool> deviceUpdated_{false};
};

#endif /* METER_MASTER_H_ */
