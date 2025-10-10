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

  struct MpptTracker {
    double voltage{0.0};
    double current{0.0};
    double power{0.0};
    double energy{0.0};
  };

  struct AcPhase {
    double voltage{0.0};
    double current{0.0};
  };

  struct Values {
    uint64_t time{0};
    double acEnergy{0.0};
    AcPhase acPhase1;
    AcPhase acPhase2;
    AcPhase acPhase3;
    double acPowerActive{0.0};
    double acPowerApparent{0.0};
    double acPowerReactive{0.0};
    double acPowerFactor{0.0};
    double dcPower{0.0};
    double acFrequency{0.0};
    double acEfficiency{0.0};
    MpptTracker dcString1;
    MpptTracker dcString2;
    double feedInTariff{0.0};
  };

  std::string getJsonDump(void) const;
  Values getValues(void) const;

  std::expected<void, ModbusError> connect(void);
  std::expected<void, ModbusError> printModbusInfo();
  std::expected<void, ModbusError> updateValuesAndJson(void);

  void setUpdateCallback(std::function<void(const std::string &)> cb);

private:
  void runLoop();
  Inverter makeModbusConfig(const ModbusRootConfig &cfg);

  const ModbusRootConfig &cfg_;
  Inverter inverter_;
  Values values_;
  nlohmann::json json_;
  std::shared_ptr<spdlog::logger> modbusLogger_;

  // --- threading / callbacks ---
  std::function<void(const std::string &)> updateCallback_;
  SignalHandler &handler_;
  mutable std::mutex cbMutex_;
  std::thread worker_;
  std::atomic<bool> connectedAndValid_{false};
  std::condition_variable cv_;
};

#endif /* MODBUS_MASTER_H_ */
