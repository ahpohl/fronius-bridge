#ifndef METER_SLAVE_H_
#define METER_SLAVE_H_

#include "config_yaml.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <atomic>
#include <expected>
#include <fronius/modbus_error.h>
#include <memory>
#include <modbus/modbus.h>
#include <mutex>
#include <poll.h>
#include <thread>

class MeterSlave {
public:
  MeterSlave(const MeterSlaveConfig &cfg, std::string meterName,
             SignalHandler &signalHandler);
  virtual ~MeterSlave();

  // Non-copyable, non-movable — owns threads and a server socket.
  MeterSlave(const MeterSlave &) = delete;
  MeterSlave &operator=(const MeterSlave &) = delete;
  MeterSlave(MeterSlave &&) = delete;
  MeterSlave &operator=(MeterSlave &&) = delete;

  void updateValues(MeterTypes::Values values);
  void updateDevice(MeterTypes::Device device);
  static constexpr int MODBUS_REGISTERS = 65535;

private:
  std::shared_ptr<spdlog::logger> logger_;
  // Parent meter's name. Held by value: this slave outlives the YAML
  // parse, and the parent's std::vector<MeterConfig> entry may relocate.
  const std::string name_;
  // Held by value: AppConfig's std::vector<MeterConfig> may reallocate,
  // which would dangle any reference into a nested slave config.
  const MeterSlaveConfig cfg_;
  MeterTypes::ErrorAction
  handleResult(std::expected<void, ModbusError> &&result);

  // RTU and TCP listener and connection handler
  modbus_t *listenCtx_{nullptr};
  int serverSocket_{-1};
  std::expected<void, ModbusError> startListener(void);
  void rtuClientHandler(void);
  void tcpClientHandler();
  void tcpClientWorker(int clientSocket);

  // --- modbus registers and values
  std::atomic<std::shared_ptr<modbus_mapping_t>> regs_{nullptr};
  struct ModbusDeleter {
    void operator()(modbus_mapping_t *p) {
      if (p)
        modbus_mapping_free(p);
    }
  };
  bool deviceUpdated_{false};

  // --- signals / threading / callbacks ---
  SignalHandler &handler_;
  mutable std::mutex clientMutex_;
  std::thread worker_;
  std::vector<std::thread> clientThreads_;
};

#endif /* METER_SLAVE_H_ */
