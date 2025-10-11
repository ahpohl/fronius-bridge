#include "modbus_master.h"
#include "config_yaml.h"
#include "inverter.h"
#include "json_utils.h"
#include "math_utils.h"
#include "modbus_config.h"
#include <chrono>
#include <expected>
#include <mutex>
#include <nlohmann/json.hpp>

ModbusMaster::ModbusMaster(const ModbusRootConfig &cfg,
                           SignalHandler &signalHandler)
    : cfg_(cfg), inverter_(makeModbusConfig(cfg)), handler_(signalHandler) {

  modbusLogger_ = spdlog::get("modbus");
  if (!modbusLogger_)
    modbusLogger_ = spdlog::default_logger();

  // Inverter callbacks
  inverter_.setConnectCallback([this]() {
    modbusLogger_->info("Inverter connected successfully");

    auto valid = inverter_.validateDevice();
    if (!valid) {
      connectedAndValid_.store(false);
    } else {
      connectedAndValid_.store(true);
      modbusLogger_->info("The inverter is SunSpec v1.0 compatible");
      auto info = printModbusInfo();
    }
  });

  inverter_.setDisconnectCallback([this]() {
    modbusLogger_->warn("Inverter disconnected");
    connectedAndValid_ = false;
  });

  inverter_.setErrorCallback([this](const ModbusError &err) {
    if (err.severity == ModbusError::Severity::FATAL) {
      modbusLogger_->error("FATAL Modbus error: {}", err.toString());

      // FATAL error: terminate main loop
      handler_.notify();

    } else if (err.severity == ModbusError::Severity::TRANSIENT) {
      modbusLogger_->debug("Transient Modbus error: {}", err.toString());

      // signal to main loop that the device is not ready
      connectedAndValid_.store(false);
    }
  });

  // Connect inverter
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

Inverter ModbusMaster::makeModbusConfig(const ModbusRootConfig &cfg) {
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

  return Inverter(mcfg);
}

std::expected<void, ModbusError> ModbusMaster::connect() {
  // Start the inverter connection thread
  auto res = inverter_.connect();
  if (!res) {
    return std::unexpected(res.error());
  }

  // Wait for successful connection (blocks until connected)
  inverter_.waitForConnection();

  return {};
}

std::expected<void, ModbusError> ModbusMaster::printModbusInfo() {
  // --- Manufacturer and model ---
  auto mfg = inverter_.getManufacturer();
  if (!mfg) {
    return std::unexpected(mfg.error());
  } else
    modbusLogger_->info("Manufacturer: {}", mfg.value());

  auto deviceModel = inverter_.getDeviceModel();
  if (!deviceModel) {
    return std::unexpected(deviceModel.error());
  } else
    modbusLogger_->info("Model: {}", deviceModel.value());

  // --- Device serial number and firmware version
  auto serial = inverter_.getSerialNumber();
  if (!serial) {
    return std::unexpected(serial.error());
  } else
    modbusLogger_->info("Serial number: {}", serial.value());

  auto version = inverter_.getFwVersion();
  if (!version) {
    return std::unexpected(version.error());
  } else
    modbusLogger_->info("Firmware version: {}", version.value());

  // --- Inverter ID, register map type and number of phases ---
  modbusLogger_->info(
      "Inverter ID {}: {} register model, {} phase{}", inverter_.getId(),
      inverter_.getUseFloatRegisters() ? "float" : "int+sf",
      inverter_.getPhases(), (inverter_.getPhases() > 1 ? "s" : ""));

  // --- Modbus slave address ---
  auto remoteSlaveId = inverter_.getModbusDeviceAddress();
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
  auto regs = inverter_.fetchInverterRegisters();
  if (!regs) {
    return std::unexpected(regs.error());
  }

  Values values{};
  values.time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  values.ac.energy = inverter_.getAcEnergy() * 1e-3;

  // Phase 1
  values.ac.phase1.voltage = inverter_.getAcVoltage(Fronius::Phase::PHA);
  values.ac.phase1.current = inverter_.getAcCurrent(Fronius::Phase::PHA);

  // Phase 2 (if available)
  int numPhases = inverter_.getPhases();
  if (numPhases > 1) {
    values.ac.phase2.voltage = inverter_.getAcVoltage(Fronius::Phase::PHB);
    values.ac.phase2.current = inverter_.getAcCurrent(Fronius::Phase::PHB);
  }

  // Phase 3 (if available)
  if (inverter_.getPhases() > 2) {
    values.ac.phase3.voltage = inverter_.getAcVoltage(Fronius::Phase::PHC);
    values.ac.phase3.current = inverter_.getAcCurrent(Fronius::Phase::PHC);
  }

  // Power
  values.ac.power.active = inverter_.getAcPowerActive();
  values.ac.power.apparent = inverter_.getAcPowerApparent();
  values.ac.power.reactive = inverter_.getAcPowerReactive();
  values.dc.power = inverter_.getDcPower();

  values.ac.frequency = inverter_.getAcFrequency();
  values.ac.efficiency =
      safeDivide(values.ac.power.active, values.dc.power, modbusLogger_.get(),
                 "AC efficiency: division by zero or near-zero DC power") *
      100;
  values.feedInTariff = cfg_.feedInTariff;

  // --- create JSON string ---

  nlohmann::ordered_json newJson;

  // Create JSON array for phases
  json phases = json::array();

  // Phase 1
  phases.push_back({
      {"id", 1},
      {"current", PreciseDouble{values.ac.phase1.current, 3}},
      {"voltage", PreciseDouble{values.ac.phase1.voltage, 2}},
  });

  // Phase 2 (if available)
  if (numPhases > 1) {
    phases.push_back({
        {"id", 2},
        {"current", PreciseDouble{values.ac.phase2.current, 3}},
        {"voltage", PreciseDouble{values.ac.phase2.voltage, 2}},
    });
  }

  // Phase 3 (if available)
  if (numPhases > 2) {
    phases.push_back({
        {"id", 3},
        {"current", PreciseDouble{values.ac.phase3.current, 3}},
        {"voltage", PreciseDouble{values.ac.phase3.voltage, 2}},
    });
  }

  // Power
  json power = {
      {"active", PreciseDouble{values.ac.power.active, 1}},
      {"apparent", PreciseDouble{values.ac.power.apparent, 1}},
      {"reactive", PreciseDouble{values.ac.power.reactive, 1}},
      {"factor", PreciseDouble{values.ac.power.factor, 2}},
  };

  newJson["time"] = std::to_string(values.time);
  newJson["feed_in_tariff"] = PreciseDouble{values.feedInTariff, 4};

  newJson["ac"] = {
      {"energy", PreciseDouble{values.ac.energy, 1}},
      {"phases", phases},
      {"power", power},
      {"frequency", PreciseDouble{values.ac.frequency, 2}},
      {"efficiency", PreciseDouble{values.ac.efficiency, 1}},
  };

  // ---- Strings array ----
  json inputs = json::array();

  // String 1
  inputs.push_back({
      {"id", 1},
      {"voltage", PreciseDouble{values.dc.input1.voltage, 0}},
      {"current", PreciseDouble{values.dc.input1.current, 0}},
      {"power", PreciseDouble{values.dc.input1.power, 0}},
      {"energy", PreciseDouble{values.dc.input1.energy, 0}},
  });

  // String 2 (if available)
  int numInputs = 2;
  if (numInputs > 1) {
    inputs.push_back({
        {"id", 2},
        {"voltage", PreciseDouble{values.dc.input2.voltage, 0}},
        {"current", PreciseDouble{values.dc.input2.current, 0}},
        {"power", PreciseDouble{values.dc.input2.power, 0}},
        {"energy", PreciseDouble{values.dc.input2.energy, 0}},
    });
  }

  newJson["dc"] = {
      {"power", PreciseDouble{values.dc.power, 1}},
      {"inputs", inputs},
  };

  // Update shared JSON with lock
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    json_ = std::move(newJson);
  }

  modbusLogger_->debug("{}", json_.dump());
  return {};
}
