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
    double voltage{0.0};
    double current{0.0};
    double power{0.0};
    double energy{0.0};
  };

  struct Dc {
    double power{0.0};
    Input input1;
    Input input2;
  };

  struct Phase {
    double voltage{0.0};
    double current{0.0};
  };

  struct Power {
    double active{0.0};
    double apparent{0.0};
    double reactive{0.0};
    double factor{0.0};
  };

  struct Ac {
    double energy{0.0};
    Phase phase1;
    Phase phase2;
    Phase phase3;
    Power power;
    double frequency{0.0};
    double efficiency{0.0};
  };

  struct Values {
    uint64_t time{0};
    double feedInTariff{0.0};
    struct Ac ac;
    struct Dc dc;
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
  nlohmann::ordered_json json_;
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
