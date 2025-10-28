#include "modbus_master.h"
#include "config_yaml.h"
#include "fronius_types.h"
#include "inverter.h"
#include "json_utils.h"
#include "modbus_config.h"
#include <chrono>
#include <expected>
#include <modbus_error.h>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

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
      connected_.store(false);
    } else {
      modbusLogger_->info("The inverter is SunSpec v1.0 compatible");
      auto info = printModbusInfo();
      connected_.store(true);
    }
  });

  inverter_.setDisconnectCallback([this](int delay) {
    modbusLogger_->warn(
        "Inverter disconnected, trying to reconnect in {} {}...", delay,
        delay == 1 ? "second" : "seconds");
  });

  inverter_.setErrorCallback([this](const ModbusError &err) {
    if (err.severity == ModbusError::Severity::FATAL) {
      modbusLogger_->error("FATAL Modbus error: {}: {} (code {})", err.message,
                           modbus_strerror(err.code), err.code);

      // FATAL error: terminate main loop
      handler_.shutdown();

    } else if (err.severity == ModbusError::Severity::TRANSIENT) {
      modbusLogger_->debug("Transient Modbus error: {}: {} (code {})",
                           err.message, modbus_strerror(err.code), err.code);

      // Mark the connection as disconnected and try to reconnect
      connected_.store(false);
      inverter_.triggerReconnect();
    }
  });

  // Start inverter connect loop
  inverter_.connect();

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

    if (connected_.load()) {

      // --- Update values ---
      auto update = updateValuesAndJson();
      if (!update) {
        connected_.store(false);
      } else {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (updateCallback_) {
          try {
            updateCallback_(jsonValues_.dump());
          } catch (const std::exception &ex) {
            modbusLogger_->error(
                "FATAL error in ModbusMaster update callback: {}", ex.what());
            handler_.shutdown();
          }
        }
      }

      // --- Update events ---
      auto events = updateEventsAndJson();
      if (!events) {
        connected_.store(false);
      } else {
        std::lock_guard<std::mutex> lock(cbMutex_);
        if (eventCallback_) {
          try {
            eventCallback_(jsonEvents_.dump());
          } catch (const std::exception &ex) {
            modbusLogger_->error(
                "FATAL error in ModbusMaster event callback: {}", ex.what());
            handler_.shutdown();
          }
        }
      }
    }

    // --- Wait for next update interval ---
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

void ModbusMaster::setEventCallback(
    std::function<void(const std::string &)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  eventCallback_ = std::move(cb);
}

std::string ModbusMaster::getJsonDump() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return jsonValues_.dump();
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

  // Response timeout parameters
  mcfg.secTimeout = cfg.responseTimeout->sec;
  mcfg.usecTimeout = cfg.responseTimeout->usec;

  // Reconnect parameters
  mcfg.reconnectDelay = cfg.reconnectDelay->min;
  mcfg.reconnectDelayMax = cfg.reconnectDelay->max;
  mcfg.exponential = cfg.reconnectDelay->exponential;

  return Inverter(mcfg);
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
  modbusLogger_->info("Inverter ID {}: {} register model", inverter_.getId(),
                      inverter_.getUseFloatRegisters() ? "float" : "int+sf");
  modbusLogger_->info(
      "Configuration: {} phase{}, {} input{}, {}", inverter_.getPhases(),
      (inverter_.getPhases() > 1 ? "s" : ""), inverter_.getInputs(),
      (inverter_.getInputs() > 1 ? "s" : ""),
      inverter_.isHybrid() ? "hybrid" : "non-hybrid");

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

  try {
    // AC values
    values.acEnergy =
        ModbusError::ModbusError::getOrThrow(inverter_.getAcEnergy()) * 1e-3;
    values.acPowerActive =
        ModbusError::getOrThrow(inverter_.getAcPowerActive());
    values.acPowerApparent =
        ModbusError::getOrThrow(inverter_.getAcPowerApparent());
    values.acPowerReactive =
        ModbusError::getOrThrow(inverter_.getAcPowerReactive());
    values.acPowerFactor =
        ModbusError::getOrThrow(inverter_.getAcPowerFactor());

    // Phase 1
    values.phase1.acVoltage =
        ModbusError::getOrThrow(inverter_.getAcVoltage(FroniusTypes::Phase::A));
    values.phase1.acCurrent =
        ModbusError::getOrThrow(inverter_.getAcCurrent(FroniusTypes::Phase::A));

    // Phase 2
    if (inverter_.getPhases() > 1) {
      values.phase2.acVoltage = ModbusError::getOrThrow(
          inverter_.getAcVoltage(FroniusTypes::Phase::B));
      values.phase2.acCurrent = ModbusError::getOrThrow(
          inverter_.getAcCurrent(FroniusTypes::Phase::B));
    }

    // Phase 3
    if (inverter_.getPhases() > 2) {
      values.phase3.acVoltage = ModbusError::getOrThrow(
          inverter_.getAcVoltage(FroniusTypes::Phase::C));
      values.phase3.acCurrent = ModbusError::getOrThrow(
          inverter_.getAcCurrent(FroniusTypes::Phase::C));
    }

    values.acFrequency = ModbusError::getOrThrow(inverter_.getAcFrequency());

    // DC values
    values.dcPower = ModbusError::getOrThrow(
        inverter_.getDcPower(FroniusTypes::Input::TOTAL));
    values.input1.dcPower =
        ModbusError::getOrThrow(inverter_.getDcPower(FroniusTypes::Input::A));
    values.input1.dcVoltage =
        ModbusError::getOrThrow(inverter_.getDcVoltage(FroniusTypes::Input::A));
    values.input1.dcCurrent =
        ModbusError::getOrThrow(inverter_.getDcCurrent(FroniusTypes::Input::A));
    if (!inverter_.isHybrid())
      values.input1.dcEnergy = ModbusError::getOrThrow(inverter_.getDcEnergy(
                                   FroniusTypes::Input::A)) *
                               1e-3;

    if (inverter_.getInputs() == 2) {
      values.input2.dcPower =
          ModbusError::getOrThrow(inverter_.getDcPower(FroniusTypes::Input::B));
      values.input2.dcVoltage = ModbusError::getOrThrow(
          inverter_.getDcVoltage(FroniusTypes::Input::B));
      values.input2.dcCurrent = ModbusError::getOrThrow(
          inverter_.getDcCurrent(FroniusTypes::Input::B));
      if (!inverter_.isHybrid())
        values.input2.dcEnergy = ModbusError::getOrThrow(inverter_.getDcEnergy(
                                     FroniusTypes::Input::B)) *
                                 1e-3;
    }

  } catch (const ModbusError &err) {
    modbusLogger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  if (std::abs(values.dcPower) > 1e-12) {
    values.efficiency = values.acPowerActive / values.dcPower * 100.0;
  } else {
    values.efficiency = 0.0;
  }

  // ---- Build JSON ----
  json newJson;

  newJson["time"] = values.time;
  newJson["ac_energy"] = JsonUtils::roundTo(values.acEnergy, 1);

  // AC power metrics
  newJson["ac_power_active"] = JsonUtils::roundTo(values.acPowerActive, 1);
  newJson["ac_power_apparent"] = JsonUtils::roundTo(values.acPowerApparent, 1);
  newJson["ac_power_reactive"] = JsonUtils::roundTo(values.acPowerReactive, 1);
  newJson["ac_power_factor"] = JsonUtils::roundTo(values.acPowerFactor, 1);

  // ---- Phases ----
  json phases = json::array();
  std::array<const Phase *, 3> phaseList = {&values.phase1, &values.phase2,
                                            &values.phase3};

  int phaseCount =
      std::clamp(inverter_.getPhases(), 1, static_cast<int>(phaseList.size()));

  for (int i = 0; i < phaseCount; ++i) {
    phases.push_back({
        {"id", i + 1},
        {"ac_voltage", JsonUtils::roundTo(phaseList[i]->acVoltage, 2)},
        {"ac_current", JsonUtils::roundTo(phaseList[i]->acCurrent, 3)},
    });
  }

  newJson["phases"] = std::move(phases);
  newJson["ac_frequency"] = JsonUtils::roundTo(values.acFrequency, 2);
  newJson["dc_power"] = JsonUtils::roundTo(values.dcPower, 1);
  newJson["efficiency"] = JsonUtils::roundTo(values.efficiency, 1);

  // ---- DC Inputs ----
  json inputs = json::array();
  std::array<const Input *, 2> inputList = {&values.input1, &values.input2};

  int inputCount =
      std::clamp(inverter_.getInputs(), 1, static_cast<int>(inputList.size()));

  for (int i = 0; i < inputCount; ++i) {
    json input = {
        {"id", i + 1},
        {"dc_voltage", JsonUtils::roundTo(inputList[i]->dcVoltage, 2)},
        {"dc_current", JsonUtils::roundTo(inputList[i]->dcCurrent, 3)},
        {"dc_power", JsonUtils::roundTo(inputList[i]->dcPower, 1)},
    };

    if (!inverter_.isHybrid()) {
      input["dc_energy"] = JsonUtils::roundTo(inputList[i]->dcEnergy, 1);
    }

    inputs.push_back(std::move(input));
  }

  newJson["inputs"] = std::move(inputs);

  // ---- Commit values ----
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    jsonValues_ = std::move(newJson);
    values_ = std::move(values);
  }

  modbusLogger_->debug("{}", jsonValues_.dump());

  return {};
}

std::expected<void, ModbusError> ModbusMaster::updateEventsAndJson() {
  Events newEvents;

  try {
    newEvents.activeCode = inverter_.getActiveStateCode();
    newEvents.state = ModbusError::getOrThrow(inverter_.getState());
    newEvents.events = ModbusError::getOrThrow(inverter_.getEvents());
  } catch (const ModbusError &err) {
    modbusLogger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  // ---- Build JSON ----
  json newJson;

  newJson["active_code"] = newEvents.activeCode;
  newJson["state"] = newEvents.state;
  newJson["events"] = nlohmann::json::array();
  for (const auto &e : newEvents.events) {
    newJson["events"].push_back(e);
  }

  // ---- Commit events ----
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    jsonEvents_ = std::move(newJson);
    events_ = std::move(newEvents);
  }

  modbusLogger_->debug("{}", jsonEvents_.dump());

  return {};
}
