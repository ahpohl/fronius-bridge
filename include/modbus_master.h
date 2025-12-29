#ifndef MODBUS_MASTER_H_
#define MODBUS_MASTER_H_

#include "config_yaml.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <fronius/inverter.h>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <thread>

class ModbusMaster {
public:
  explicit ModbusMaster(const ModbusRootConfig &cfg,
                        SignalHandler &signalHandler);
  virtual ~ModbusMaster();

  // --- data structs ---
  struct Input {
    double dcVoltage{0.0};
    double dcCurrent{0.0};
    double dcPower{0.0};
    double dcEnergy{0.0};
  };

  struct Phase {
    double acVoltage{0.0};
    double acCurrent{0.0};
  };

  struct Values {
    uint64_t time{0};
    double acEnergy{0.0};
    double acPowerActive{0.0};
    double acPowerApparent{0.0};
    double acPowerReactive{0.0};
    double acPowerFactor{0.0};
    Phase phase1;
    Phase phase2;
    Phase phase3;
    double acFrequency{0.0};
    double dcPower{0.0};
    double efficiency{0.0};
    Input input1;
    Input input2;
  };

  struct Events {
    int activeCode{0};               // Fronius F_Active_State_Code
    std::string state;               // Inverter StVnd
    std::vector<std::string> events; // Inverter EvtVnd1-3
  };

  struct Device {
    int id{0};
    std::string manufacturer;
    std::string model;
    std::string serialNumber;
    std::string fwVersion;
    std::string dataManagerVersion;
    std::string registerModel;
    bool isHybrid{false};
    int phases{0};
    int inputs{0};
    int slaveID{0};
    double acPowerApparent{0.0};
  };

  std::string getJsonDump(void) const;
  Values getValues(void) const;

  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateEventsAndJson(void);
  std::expected<void, ModbusError> updateDeviceAndJson(void);

  void setValueCallback(std::function<void(std::string)> cb);
  void setEventCallback(std::function<void(std::string)> cb);
  void setDeviceCallback(std::function<void(std::string)> cb);
  void setAvailabilityCallback(std::function<void(std::string)> cb);

private:
  void runLoop();
  Inverter makeModbusConfig(const ModbusRootConfig &cfg);
  Inverter inverter_;
  const ModbusRootConfig &cfg_;
  std::shared_ptr<spdlog::logger> modbusLogger_;

  // --- values and events
  Device device_;
  Values values_;
  Events events_;
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

#endif /* MODBUS_MASTER_H_ */
