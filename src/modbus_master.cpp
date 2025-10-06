#include "modbus_master.h"
#include "config_yaml.h"
#include "json_utils.h"
#include "meter.h"
#include "modbus_config.h"
#include <chrono>
#include <expected>
#include <mutex>
#include <nlohmann/json.hpp>

ModbusMaster::ModbusMaster(const ModbusRootConfig &cfg,
                           SignalHandler &signalHandler)
    : cfg_(cfg), meter_(makeMeterFromConfig(cfg)), handler_(signalHandler) {

  modbusLogger_ = spdlog::get("modbus");
  if (!modbusLogger_)
    modbusLogger_ = spdlog::default_logger();

  // Meter callbacks
  meter_.setConnectCallback([this]() {
    modbusLogger_->info("Meter connected successfully");

    auto valid = meter_.validateDevice();
    if (!valid) {
      connectedAndValid_.store(false);
    } else {
      connectedAndValid_.store(true);
      modbusLogger_->info("The energy meter is SunSpec v1.0 compatible");
      auto info = printModbusInfo();
    }
  });

  meter_.setDisconnectCallback([this]() {
    modbusLogger_->warn("Meter disconnected");
    connectedAndValid_ = false;
  });

  meter_.setErrorCallback([this](const ModbusError &err) {
    if (err.severity == ModbusError::Severity::FATAL) {
      modbusLogger_->error("FATAL Modbus error: {} (code {})", err.message,
                           err.code);

      // FATAL error: terminate main loop
      handler_.notify();

    } else if (err.severity == ModbusError::Severity::TRANSIENT) {
      modbusLogger_->debug("Transient Modbus error: {} (code {})", err.message,
                           err.code);

      // signal to main loop that the device is not ready
      connectedAndValid_.store(false);
    }
  });

  // Connect meter
  auto conn = connect();
  if (!conn) {
    throw std::runtime_error(conn.error().message);
  }

  // Start update loop thread
  worker_ = std::thread(&ModbusMaster::runLoop, this);
}

ModbusMaster::~ModbusMaster() {
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void ModbusMaster::runLoop() {
  while (handler_.isRunning()) {

    if (connectedAndValid_.load()) {
      auto update = updateValuesAndJson();
      if (!update)
        connectedAndValid_.store(false);
      else {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (updateCallback_) {
          try {
            updateCallback_(json_.dump());
          } catch (const std::exception &ex) {
            modbusLogger_->error(
                "FATAL error in ModbusMaster update callback: {}", ex.what());
            handler_.notify();
          }
        }
      }
    }

    std::unique_lock<std::mutex> lock(cbMutex_);
    cv_.wait_for(lock, std::chrono::seconds(cfg_.updateInterval),
                 [this] { return !handler_.isRunning(); });
  }

  modbusLogger_->debug("Modbus master run loop stopped.");
}

void ModbusMaster::setUpdateCallback(
    std::function<void(const std::string &)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  updateCallback_ = std::move(cb);
}

std::string ModbusMaster::getJsonDump() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return json_.dump();
}

ModbusMaster::Values ModbusMaster::getValues() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return values_;
}

Meter ModbusMaster::makeMeterFromConfig(const ModbusRootConfig &cfg) {
  ModbusConfig mcfg;

  if (cfg.tcp) {
    mcfg.useTcp = true;
    mcfg.host = cfg.tcp->host;
    mcfg.port = cfg.tcp->port;
  } else if (cfg.rtu) {
    mcfg.useTcp = false;
    mcfg.device = cfg.rtu->device;
    mcfg.baud = cfg.rtu->baud;
  }

  // Enable debug only if logger is at trace level
  auto modbusLogger = spdlog::get("modbus");
  mcfg.debug = modbusLogger && (modbusLogger->level() == spdlog::level::trace);

  mcfg.slaveId = cfg.slaveId;

  // Reconnect parameters
  mcfg.reconnectDelay = cfg.reconnectDelay->min;
  mcfg.reconnectDelayMax = cfg.reconnectDelay->max;
  mcfg.exponential = cfg.reconnectDelay->exponential;

  return Meter(mcfg);
}

std::expected<void, ModbusError> ModbusMaster::connect() {
  // Start the Meter connection thread
  auto res = meter_.connect();
  if (!res) {
    return std::unexpected(res.error());
  }

  // Wait for successful connection (blocks until connected)
  meter_.waitForConnection();

  return {};
}

std::expected<void, ModbusError> ModbusMaster::printModbusInfo() {
  // --- Manufacturer and model ---
  auto mfg = meter_.getManufacturer();
  if (!mfg) {
    return std::unexpected(mfg.error());
  } else
    modbusLogger_->info("Manufacturer: {}", mfg.value());

  auto deviceModel = meter_.getDeviceModel();
  if (!deviceModel) {
    return std::unexpected(deviceModel.error());
  } else
    modbusLogger_->info("Model: {}", deviceModel.value());

  // --- Device serial number and firwmare version
  auto serial = meter_.getSerialNumber();
  if (!serial) {
    return std::unexpected(serial.error());
  } else
    modbusLogger_->info("Serial number: {}", serial.value());

  auto version = meter_.getFwVersion();
  if (!version) {
    return std::unexpected(version.error());
  } else
    modbusLogger_->info("Firmware version: {}", version.value());

  // --- Meter ID, register map type and number of phases ---
  modbusLogger_->info("Meter ID {}: {} register model, {} phase{}",
                      meter_.getId(),
                      meter_.getUseFloatRegisters() ? "float" : "int+sf",
                      meter_.getPhases(), (meter_.getPhases() > 1 ? "s" : ""));

  // --- Modbus slave address ---
  auto remoteSlaveId = meter_.getModbusDeviceAddress();
  if (!remoteSlaveId) {
    return std::unexpected(remoteSlaveId.error());
  } else {
    // Compare with configured slave ID
    if (cfg_.slaveId != remoteSlaveId.value()) {
      modbusLogger_->warn("Configured slave ID ({}) does not match "
                          "device-reported slave ID ({})",
                          cfg_.slaveId, remoteSlaveId.value());
    }
  }

  return {};
}

std::expected<void, ModbusError> ModbusMaster::updateValuesAndJson() {
  auto regs = meter_.fetchMeterRegisters();
  if (!regs) {
    return std::unexpected(regs.error());
  }

  Values values{};

  values.time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  values.energy = meter_.getAcEnergyActiveImport(Meter::Phase::TOTAL) * 1e-3;
  values.powerTotal = meter_.getAcPowerActive(Fronius::Phase::TOTAL);
  values.powerPhase1 = meter_.getAcPowerActive(Fronius::Phase::PHA);
  values.powerPhase2 = meter_.getAcPowerActive(Fronius::Phase::PHB);
  values.powerPhase3 = meter_.getAcPowerActive(Fronius::Phase::PHC);
  values.voltagePhase1 = meter_.getAcVoltage(Fronius::Phase::PHA);
  values.voltagePhase2 = meter_.getAcVoltage(Fronius::Phase::PHB);
  values.voltagePhase3 = meter_.getAcVoltage(Fronius::Phase::PHC);

  nlohmann::json newJson;

  newJson["time"] = std::to_string(values.time);
  newJson["energy"] = PreciseDouble{values.energy, 1};
  newJson["power_total"] = PreciseDouble{values.powerTotal, 0};
  newJson["power_ph1"] = PreciseDouble{values.powerPhase1, 0};
  newJson["power_ph2"] = PreciseDouble{values.powerPhase2, 0};
  newJson["power_ph3"] = PreciseDouble{values.powerPhase3, 0};
  newJson["voltage_ph1"] = PreciseDouble{values.voltagePhase1, 2};
  newJson["voltage_ph2"] = PreciseDouble{values.voltagePhase2, 2};
  newJson["voltage_ph3"] = PreciseDouble{values.voltagePhase3, 2};

  // Update shared JSON with lock
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    json_ = std::move(newJson);
  }

  modbusLogger_->debug("{}", json_.dump());
  return {};
}