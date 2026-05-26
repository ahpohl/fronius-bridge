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

class MeterMaster {
public:
  explicit MeterMaster(const MeterConfig &cfg, SignalHandler &signalHandler,
                       std::shared_ptr<FroniusBus> bus);
  virtual ~MeterMaster();

  // Non-copyable, non-movable — owns a thread.
  MeterMaster(const MeterMaster &) = delete;
  MeterMaster &operator=(const MeterMaster &) = delete;
  MeterMaster(MeterMaster &&) = delete;
  MeterMaster &operator=(MeterMaster &&) = delete;

  std::string getJsonDump(void) const;
  MeterTypes::Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  // Returns true if the device info was (re)read from the wire on this
  // call, false if it was already cached from a previous call and nothing
  // changed. Used by runLoop to skip publishing the (unchanged) device
  // JSON to MQTT on every poll cycle.
  std::expected<bool, ModbusError> updateDeviceAndJson(void);

  void
  setValueCallback(std::function<void(std::string, MeterTypes::Values)> cb);
  void
  setDeviceCallback(std::function<void(std::string, MeterTypes::Device)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

  static ModbusBusConfig makeBusConfig(const MeterConfig &cfg);

private:
  static ModbusDeviceConfig makeDeviceConfig(const MeterConfig &cfg);
  void runLoop();

  std::shared_ptr<FroniusBus> bus_;
  std::shared_ptr<Meter> meter_;
  // Held by value — with std::vector<MeterConfig> in AppConfig, a reference
  // would dangle on vector reallocation. The config is small and copyable.
  const MeterConfig cfg_;
  std::shared_ptr<spdlog::logger> logger_;

  // IDs of bus-level callbacks this master has registered. Removed in the
  // destructor before any state captured by those callbacks (this,
  // modbusLogger_, handler_) is torn down. With a shared bus, the bus
  // thread can outlive any individual master; leaving the callbacks
  // attached would be a use-after-free hazard the next time the bus
  // fired one.
  std::vector<FroniusBus::CallbackId> busCallbackIds_;

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
