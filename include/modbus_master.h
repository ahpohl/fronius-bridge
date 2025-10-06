#ifndef MODBUS_MASTER_H_
#define MODBUS_MASTER_H_

#include "config_yaml.h"
#include "meter.h"
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

  struct Values {
    uint64_t time{0};
    double energy{0.0};
    double powerTotal{0.0};
    double powerPhase1{0.0};
    double powerPhase2{0.0};
    double powerPhase3{0.0};
    double voltagePhase1{0.0};
    double voltagePhase2{0.0};
    double voltagePhase3{0.0};
  };

  std::string getJsonDump(void) const;
  Values getValues(void) const;

  std::expected<void, ModbusError> connect(void);
  std::expected<void, ModbusError> printModbusInfo();
  std::expected<void, ModbusError> updateValuesAndJson(void);

  void setUpdateCallback(std::function<void(const std::string &)> cb);

private:
  void runLoop();
  Meter makeMeterFromConfig(const ModbusRootConfig &cfg);

  const ModbusRootConfig &cfg_;
  Meter meter_;
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
