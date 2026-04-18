#ifndef METER_MASTER_H_
#define METER_MASTER_H_

#include "config_yaml.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <expected>
#include <fronius/meter.h>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <thread>

class MeterMaster {
public:
  explicit MeterMaster(const MeterMasterConfig &cfg,
                       SignalHandler &signalHandler);
  virtual ~MeterMaster();

  std::string getJsonDump(void) const;
  MeterTypes::Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateEventsAndJson(void);
  std::expected<void, ModbusError> updateDeviceAndJson(void);

  void
  setValueCallback(std::function<void(std::string, MeterTypes::Values)> cb);
  void
  setDeviceCallback(std::function<void(std::string, MeterTypes::Device)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

private:
  void runLoop();
  Meter makeModbusConfig(const MeterMasterConfig &cfg);
  Meter meter_;
  const MeterMasterConfig &cfg_;
  std::shared_ptr<spdlog::logger> modbusLogger_;

  // --- values and events
  MeterTypes::Device device_;
  MeterTypes::Values values_;
  nlohmann::ordered_json jsonValues_;
  nlohmann::ordered_json jsonEvents_;
  nlohmann::json jsonDevice_;
  std::optional<std::size_t> lastEventsHash_;

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
