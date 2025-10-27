#ifndef MODBUS_MASTER_H_
#define MODBUS_MASTER_H_

#include "config_yaml.h"
#include "inverter.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <expected>
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
    int code;                                // Fronius F_Active_State_Code
    std::string state;                       // Inverter StVnd
    std::vector<std::string> acEvents;       // Inverter Evt1
    std::vector<std::string> dcEvents;       // MPPT Evt
    std::vector<std::string> acVendorEvents; // Inverter EvtVnd1-3
  };

  std::string getJsonDump(void) const;
  Values getValues(void) const;

  std::expected<void, ModbusError> printModbusInfo();
  std::expected<void, ModbusError> updateValuesAndJson(void);
  std::expected<void, ModbusError> updateEventsAndJson(void);

  void setUpdateCallback(std::function<void(const std::string &)> cb);

private:
  void runLoop();
  Inverter makeModbusConfig(const ModbusRootConfig &cfg);

  const ModbusRootConfig &cfg_;
  Inverter inverter_;
  Values values_;
  nlohmann::ordered_json jsonValues_;
  Events events_;
  nlohmann::ordered_json jsonEvents_;
  std::shared_ptr<spdlog::logger> modbusLogger_;

  // --- threading / callbacks ---
  std::function<void(const std::string &)> updateCallback_;
  SignalHandler &handler_;
  mutable std::mutex cbMutex_;
  std::thread worker_;
  std::atomic<bool> connected_{false};
  std::condition_variable cv_;
};

#endif /* MODBUS_MASTER_H_ */
