#include "meter_master.h"
#include "config.h"
#include "config_yaml.h"
#include "json_utils.h"
#include "meter_types.h"
#include <chrono>
#include <cmath>
#include <expected>
#include <fronius/fronius_types.h>
#include <fronius/inverter.h>
#include <fronius/modbus_config.h>
#include <fronius/modbus_error.h>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sys/socket.h>

using json = nlohmann::ordered_json;

MeterMaster::MeterMaster(const MeterMasterConfig &cfg,
                         SignalHandler &signalHandler)
    : cfg_(cfg), meter_(makeModbusConfig(cfg)), handler_(signalHandler) {

  modbusLogger_ = spdlog::get("meter.master");
  if (!modbusLogger_)
    modbusLogger_ = spdlog::default_logger();

  // Meter callbacks
  meter_.setConnectCallback([this](FroniusTypes::RegisterMap map) {
    if (cfg_.tcp) {
      auto remote = meter_.getRemoteEndpoint();
      modbusLogger_->info("Meter connected to {}:{}", remote.ip, remote.port);
    } else {
      modbusLogger_->info("Meter connected at {}", cfg_.rtu->device);
    }
    modbusLogger_->debug("{} register map detected",
                         FroniusTypes::toString(map));

    connected_.store(true);

    if (availabilityCallback_)
      availabilityCallback_("connected");
  });

  meter_.setDisconnectCallback([this](int delay) {
    modbusLogger_->warn("Meter disconnected, trying to reconnect in {} {}...",
                        delay, delay == 1 ? "second" : "seconds");

    connected_.store(false); // Explicit state update

    if (availabilityCallback_)
      availabilityCallback_(connected_.load() ? "connected" : "disconnected");
  });

  meter_.setErrorCallback([this](const ModbusError &err) {
    if (err.severity == ModbusError::Severity::FATAL) {
      // Fatal error occurred - initiate shutdown sequence
      modbusLogger_->error("FATAL Modbus error: {}", err.describe());
      handler_.shutdown();

    } else if (err.severity == ModbusError::Severity::TRANSIENT) {
      // Temporary error - disconnect and reconnect
      modbusLogger_->debug("Transient Modbus error: {}", err.describe());
      connected_.store(false);
      meter_.triggerReconnect();

    } else if (err.severity == ModbusError::Severity::SHUTDOWN) {
      // Shutdown already in progress - just exit cleanly
      modbusLogger_->trace("Modbus operation cancelled due to shutdown: {}",
                           err.describe());
      connected_.store(false);
    }
  });

  // Start inverter connect loop
  meter_.connect();

  // Start update loop thread
  worker_ = std::thread(&MeterMaster::runLoop, this);
}

MeterMaster::~MeterMaster() {
  connected_.store(false);
  if (availabilityCallback_) {
    availabilityCallback_(connected_.load() ? "connected" : "disconnected");
  }

  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();

  modbusLogger_->info("Meter disconnected");
}

void MeterMaster::runLoop() {
  while (handler_.isRunning()) {

    if (connected_.load()) {
      {
        // --- Device (once) ---
        auto deviceResult = updateDeviceAndJson();
        if (!deviceResult) {
          connected_.store(false);
        } else if (deviceCallback_ && handler_.isRunning()) {
          std::string json;
          MeterTypes::Device dev;
          {
            std::lock_guard<std::mutex> lock(cbMutex_);
            json = jsonDevice_.dump();
            dev = device_;
          }
          deviceCallback_(std::move(json), std::move(dev));
        }

        // --- Values ---
        auto valuesResult = updateValuesAndJson();
        if (!valuesResult) {
          connected_.store(false);
        } else if (valueCallback_ && handler_.isRunning()) {
          std::string json;
          MeterTypes::Values vals;
          {
            std::lock_guard<std::mutex> lock(cbMutex_);
            json = jsonValues_.dump();
            vals = values_;
          }
          valueCallback_(std::move(json), std::move(vals));
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

void MeterMaster::setValueCallback(
    std::function<void(std::string, MeterTypes::Values)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  valueCallback_ = std::move(cb);
}

void MeterMaster::setDeviceCallback(
    std::function<void(std::string, MeterTypes::Device)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  deviceCallback_ = std::move(cb);
}

void MeterMaster::setAvailabilityCallback(std::function<void(std::string)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  availabilityCallback_ = std::move(cb);
}

std::string MeterMaster::getJsonDump() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return jsonValues_.dump();
}

MeterTypes::Values MeterMaster::getValues() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return values_;
}

Meter MeterMaster::makeModbusConfig(const MeterMasterConfig &cfg) {
  ModbusConfig mcfg;

  if (cfg.tcp) {
    mcfg.transport = ModbusTcpTransport{
        .host = cfg.tcp->host,
        .port = cfg.tcp->port,
    };
  } else if (cfg.rtu) {
    mcfg.transport = ModbusRtuTransport{
        .device = cfg.rtu->device,
        .baud = cfg.rtu->baud,
        .dataBits = cfg.rtu->dataBits,
        .stopBits = cfg.rtu->stopBits,
        .parity = parityToChar(cfg.rtu->parity),
    };
  } else {
    throw std::runtime_error("MeterMaster: no transport configured");
  }

  // Enable debug only if logger is at trace level
  auto modbusLogger = spdlog::get("meter.master");
  mcfg.debug = modbusLogger && (modbusLogger->level() == spdlog::level::trace);

  mcfg.slaveId = cfg.slaveId;

  // Response timeout parameters
  mcfg.secTimeout = cfg.responseTimeout.sec;
  mcfg.usecTimeout = cfg.responseTimeout.usec;

  // Reconnect parameters
  mcfg.reconnectDelay = cfg.reconnectDelay.min;
  mcfg.reconnectDelayMax = cfg.reconnectDelay.max;
  mcfg.exponential = cfg.reconnectDelay.exponential;

  return Meter(mcfg);
}

std::expected<void, ModbusError> MeterMaster::updateValuesAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateValuesAndJson(): Shutdown in progress"));
  }

  auto regs = meter_.fetchMeterRegisters();
  if (!regs) {
    return std::unexpected(regs.error());
  }

  MeterTypes::Values values{};

  values.time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  try {
    // energy
    values.activeEnergyImport = ModbusError::getOrThrow(
        meter_.getAcEnergyActive(FroniusTypes::EnergyDirection::IMPORT));
    values.activeEnergyExport = ModbusError::getOrThrow(
        meter_.getAcEnergyActive(FroniusTypes::EnergyDirection::EXPORT));

    // active power
    values.activePower = ModbusError::getOrThrow(
        meter_.getAcPowerActive(FroniusTypes::Phase::TOTAL));

    // apparent power
    values.apparentPower = ModbusError::getOrThrow(
        meter_.getAcPowerApparent(FroniusTypes::Phase::TOTAL));

    // reactive power
    values.reactivePower = ModbusError::getOrThrow(
        meter_.getAcPowerReactive(FroniusTypes::Phase::TOTAL));

    // power factor
    values.powerFactor = ModbusError::getOrThrow(
        meter_.getAcPowerFactor(FroniusTypes::Phase::AVERAGE));

    // voltages
    values.phVoltage =
        ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::PHV));
    values.ppVoltage =
        ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::PPV));

    // frequency
    values.frequency = ModbusError::getOrThrow(meter_.getAcFrequency());

    // --- Per-phase (1 phase) ---
    values.phase1.activePower = ModbusError::getOrThrow(
        meter_.getAcPowerActive(FroniusTypes::Phase::A));
    values.phase1.apparentPower = ModbusError::getOrThrow(
        meter_.getAcPowerApparent(FroniusTypes::Phase::A));
    values.phase1.reactivePower = ModbusError::getOrThrow(
        meter_.getAcPowerReactive(FroniusTypes::Phase::A));
    values.phase1.powerFactor = ModbusError::getOrThrow(
        meter_.getAcPowerFactor(FroniusTypes::Phase::A));
    values.phase1.phVoltage =
        ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::A));
    values.phase1.ppVoltage =
        ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::AB));
    values.phase1.current =
        ModbusError::getOrThrow(meter_.getAcCurrent(FroniusTypes::Phase::A));

    // --- Per-phase (2 phases) ---
    if (meter_.getPhases() > 1) {
      values.phase2.activePower = ModbusError::getOrThrow(
          meter_.getAcPowerActive(FroniusTypes::Phase::B));
      values.phase2.apparentPower = ModbusError::getOrThrow(
          meter_.getAcPowerApparent(FroniusTypes::Phase::B));
      values.phase2.reactivePower = ModbusError::getOrThrow(
          meter_.getAcPowerReactive(FroniusTypes::Phase::B));
      values.phase2.powerFactor = ModbusError::getOrThrow(
          meter_.getAcPowerFactor(FroniusTypes::Phase::B));
      values.phase2.phVoltage =
          ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::B));
      values.phase2.ppVoltage =
          ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::BC));
      values.phase2.current =
          ModbusError::getOrThrow(meter_.getAcCurrent(FroniusTypes::Phase::B));
    }

    // --- Per-phase (3 phases) ---
    if (meter_.getPhases() > 2) {
      values.phase3.activePower = ModbusError::getOrThrow(
          meter_.getAcPowerActive(FroniusTypes::Phase::C));
      values.phase3.apparentPower = ModbusError::getOrThrow(
          meter_.getAcPowerApparent(FroniusTypes::Phase::C));
      values.phase3.reactivePower = ModbusError::getOrThrow(
          meter_.getAcPowerReactive(FroniusTypes::Phase::C));
      values.phase3.powerFactor = ModbusError::getOrThrow(
          meter_.getAcPowerFactor(FroniusTypes::Phase::C));
      values.phase3.phVoltage =
          ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::C));
      values.phase3.ppVoltage =
          ModbusError::getOrThrow(meter_.getAcVoltage(FroniusTypes::Phase::CA));
      values.phase3.current =
          ModbusError::getOrThrow(meter_.getAcCurrent(FroniusTypes::Phase::C));
    }

    // current (uses sum of phase values for proprietary map)
    if (meter_.getRegisterMap() == FroniusTypes::RegisterMap::PROPRIETARY) {
      values.current =
          values.phase1.current + values.phase2.current + values.phase3.current;
    } else {
      values.current = ModbusError::getOrThrow(
          meter_.getAcCurrent(FroniusTypes::Phase::TOTAL));
    }

    // derived energy values — depends on what the register map provides
    if (meter_.getRegisterMap() == FroniusTypes::RegisterMap::SUNSPEC) {
      values.apparentEnergyImport = ModbusError::getOrThrow(
          meter_.getAcEnergyApparent(FroniusTypes::EnergyDirection::IMPORT));
      values.apparentEnergyExport = ModbusError::getOrThrow(
          meter_.getAcEnergyApparent(FroniusTypes::EnergyDirection::EXPORT));

      // derive reactive from apparent and active, signed by reactive power
      // direction
      const double sign = values.reactivePower >= 0.0 ? 1.0 : -1.0;
      values.reactiveEnergyImport =
          sign *
          std::sqrt(values.apparentEnergyImport * values.apparentEnergyImport -
                    values.activeEnergyImport * values.activeEnergyImport);
      values.reactiveEnergyExport =
          sign *
          std::sqrt(values.apparentEnergyExport * values.apparentEnergyExport -
                    values.activeEnergyExport * values.activeEnergyExport);

    } else if (meter_.getRegisterMap() ==
               FroniusTypes::RegisterMap::PROPRIETARY) {
      values.reactiveEnergyImport = ModbusError::getOrThrow(
          meter_.getAcEnergyReactive(FroniusTypes::EnergyDirection::IMPORT));
      values.reactiveEnergyExport = ModbusError::getOrThrow(
          meter_.getAcEnergyReactive(FroniusTypes::EnergyDirection::EXPORT));

      // derive apparent from active and reactive
      values.apparentEnergyImport =
          std::hypot(values.activeEnergyImport, values.reactiveEnergyImport);
      values.apparentEnergyExport =
          std::hypot(values.activeEnergyExport, values.reactiveEnergyExport);
    }

  } catch (const ModbusError &err) {
    modbusLogger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  // ---- Build JSON ----
  json newJson;
  json phases = json::array();

  phases.push_back({
      {"id", 1},
      {"power_active", JsonUtils::roundTo(values.phase1.activePower, 0)},
      {"power_apparent", JsonUtils::roundTo(values.phase1.apparentPower, 0)},
      {"power_reactive", JsonUtils::roundTo(values.phase1.reactivePower, 0)},
      {"power_factor", JsonUtils::roundTo(values.phase1.powerFactor, 1)},
      {"voltage_ph", JsonUtils::roundTo(values.phase1.phVoltage, 1)},
      {"voltage_pp", JsonUtils::roundTo(values.phase1.ppVoltage, 1)},
      {"current", JsonUtils::roundTo(values.phase1.current, 3)},

  });

  if (meter_.getPhases() > 1) {
    phases.push_back({
        {"id", 2},
        {"power_active", JsonUtils::roundTo(values.phase2.activePower, 0)},
        {"power_apparent", JsonUtils::roundTo(values.phase2.apparentPower, 0)},
        {"power_reactive", JsonUtils::roundTo(values.phase2.reactivePower, 0)},
        {"power_factor", JsonUtils::roundTo(values.phase2.powerFactor, 1)},
        {"voltage_ph", JsonUtils::roundTo(values.phase2.phVoltage, 1)},
        {"voltage_pp", JsonUtils::roundTo(values.phase2.ppVoltage, 1)},
        {"current", JsonUtils::roundTo(values.phase2.current, 3)},

    });
  }

  if (meter_.getPhases() > 2) {
    phases.push_back({
        {"id", 3},
        {"power_active", JsonUtils::roundTo(values.phase3.activePower, 0)},
        {"power_apparent", JsonUtils::roundTo(values.phase3.apparentPower, 0)},
        {"power_reactive", JsonUtils::roundTo(values.phase3.reactivePower, 0)},
        {"power_factor", JsonUtils::roundTo(values.phase3.powerFactor, 1)},
        {"voltage_ph", JsonUtils::roundTo(values.phase3.phVoltage, 1)},
        {"voltage_pp", JsonUtils::roundTo(values.phase3.ppVoltage, 1)},
        {"current", JsonUtils::roundTo(values.phase3.current, 3)},
    });
  }

  newJson["time"] = values.time;
  newJson["energy_active_import"] =
      JsonUtils::roundTo(values.activeEnergyImport * 1e-3, 3);
  newJson["energy_active_export"] =
      JsonUtils::roundTo(values.activeEnergyExport * 1e-3, 3);
  newJson["energy_apparent_import"] =
      JsonUtils::roundTo(values.apparentEnergyImport * 1e-3, 3);
  newJson["energy_apparent_export"] =
      JsonUtils::roundTo(values.apparentEnergyExport * 1e-3, 3);
  newJson["energy_reactive_import"] =
      JsonUtils::roundTo(values.reactiveEnergyImport * 1e-3, 3);
  newJson["energy_reactive_export"] =
      JsonUtils::roundTo(values.reactiveEnergyExport * 1e-3, 3);
  newJson["power_active"] = JsonUtils::roundTo(values.activePower, 0);
  newJson["power_apparent"] = JsonUtils::roundTo(values.apparentPower, 0);
  newJson["power_reactive"] = JsonUtils::roundTo(values.reactivePower, 0);
  newJson["power_factor"] = JsonUtils::roundTo(values.powerFactor, 1);
  newJson["frequency"] = JsonUtils::roundTo(values.frequency, 1);
  newJson["voltage_ph"] = JsonUtils::roundTo(values.phVoltage, 1);
  newJson["voltage_pp"] = JsonUtils::roundTo(values.ppVoltage, 1);
  newJson["current"] = JsonUtils::roundTo(values.current, 3);
  newJson["phases"] = phases;

  // Update shared values and JSON with lock
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    values_ = std::move(values);
    jsonValues_ = std::move(newJson);
  }

  modbusLogger_->debug("{}", jsonValues_.dump());

  return {};
}

std::expected<void, ModbusError> MeterMaster::updateDeviceAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateDeviceAndJson(): Shutdown in progress"));
  }

  if (deviceUpdated_.load())
    return {};

  MeterTypes::Device newDevice;

  try {
    newDevice.manufacturer = ModbusError::getOrThrow(meter_.getManufacturer());
    newDevice.model = ModbusError::getOrThrow(meter_.getDeviceModel());
    newDevice.serialNumber = ModbusError::getOrThrow(meter_.getSerialNumber());
    newDevice.fwVersion = ModbusError::getOrThrow(meter_.getFwVersion());
    newDevice.options = std::string(PROJECT_VERSION) + "-" + GIT_COMMIT_HASH;
    newDevice.dataManagerVersion = ModbusError::getOrThrow(meter_.getOptions());
    newDevice.registerModel =
        meter_.getUseFloatRegisters() ? "float" : "int+sf";
    newDevice.slaveID =
        ModbusError::getOrThrow(meter_.getModbusDeviceAddress());
  } catch (const ModbusError &err) {
    modbusLogger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  newDevice.id = meter_.getId();
  newDevice.phases = meter_.getPhases();

  // Compare received slave ID with configured
  if (cfg_.slaveId != newDevice.slaveID) {
    modbusLogger_->warn("Slave ID mismatch: configured {}, received {}",
                        cfg_.slaveId, newDevice.slaveID);
  }

  // ---- Build ordered JSON ----
  json newJson;

  newJson["manufacturer"] = newDevice.manufacturer;
  newJson["model"] = newDevice.model;
  newJson["serial_number"] = newDevice.serialNumber;
  newJson["firmware_version"] = newDevice.fwVersion;
  newJson["data_manager"] = newDevice.dataManagerVersion;
  newJson["register_model"] = newDevice.registerModel;
  newJson["slave_id"] = newDevice.slaveID;
  newJson["meter_id"] = newDevice.id;
  newJson["phases"] = newDevice.phases;

  modbusLogger_->debug("{}", newJson.dump());

  // ---- Commit values ----
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    jsonDevice_ = std::move(newJson);
    device_ = std::move(newDevice);
  }

  deviceUpdated_.store(true);

  return {};
}
