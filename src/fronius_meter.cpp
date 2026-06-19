#include "fronius_meter.h"
#include "config.h"
#include "config_yaml.h"
#include "meter_types.h"
#include "utils.h"
#include <chrono>
#include <cmath>
#include <expected>
#include <fronius/fronius_bus.h>
#include <fronius/fronius_types.h>
#include <fronius/modbus_config.h>
#include <fronius/modbus_error.h>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sys/socket.h>

using json = nlohmann::ordered_json;

FroniusMeter::FroniusMeter(const MeterConfig &cfg, SignalHandler &signalHandler,
                           std::shared_ptr<FroniusBus> bus)
    : bus_(std::move(bus)), cfg_(cfg),
      fcfg_(std::get<FroniusMeterConfig>(cfg.body)), handler_(signalHandler) {

  // Fixed class-based logger chain: meter.master -> meter -> default.
  // The device name is no longer part of the logger name (it already
  // appears in every connect/disconnect message), so all meter masters
  // share one configurable module.
  logger_ = spdlog::get("meter.master");
  if (!logger_)
    logger_ = spdlog::get("meter");
  if (!logger_)
    logger_ = spdlog::default_logger();

  auto devCfg = makeDeviceConfig(fcfg_);
  meter_ = std::make_shared<Meter>(bus_, devCfg);
  bus_->registerDevice(meter_);

  // --- Bus-level callbacks ---

  busCallbackIds_.push_back(bus_->addBusConnectCallback([this] {
    if (fcfg_.tcp) {
      auto remote = bus_->getRemoteEndpoint();
      logger_->info("Connected to meter '{}' at {}:{}", cfg_.name, remote.ip,
                    remote.port);
    }
  }));

  busCallbackIds_.push_back(bus_->addBusDisconnectCallback([this](int delay) {
    logger_->warn("Meter '{}' disconnected, trying to reconnect in {} {}...",
                  cfg_.name, delay, delay == 1 ? "second" : "seconds");
  }));

  busCallbackIds_.push_back(
      bus_->addBusErrorCallback([this](const ModbusError &err) {
        if (err.severity == ModbusError::Severity::FATAL) {
          logger_->error("FATAL Modbus bus error: {}", err.describe());
          handler_.shutdown();
        } else if (err.severity == ModbusError::Severity::SHUTDOWN) {
          logger_->trace("Modbus bus operation cancelled due to shutdown: {}",
                         err.describe());
        }
      }));

  // --- Device-level callbacks ---

  meter_->setDeviceReadyCallback([this](FroniusTypes::RegisterMap map) {
    logger_->debug("Meter '{}' register map: {}", cfg_.name,
                   FroniusTypes::toString(map));
    connected_.store(true);

    if (availabilityCallback_)
      availabilityCallback_("connected");
  });

  meter_->setDeviceUnavailableCallback([this] {
    connected_.store(false);

    if (availabilityCallback_)
      availabilityCallback_("disconnected");
  });

  meter_->setDeviceErrorCallback([this](const ModbusError &err) {
    if (err.severity == ModbusError::Severity::FATAL) {
      logger_->error("FATAL Modbus error: {}", err.describe());
      handler_.shutdown();

    } else if (err.severity == ModbusError::Severity::TRANSIENT) {
      logger_->debug("Transient Modbus error: {}", err.describe());
      connected_.store(false);
      bus_->scheduleDeviceRetry(meter_);

    } else if (err.severity == ModbusError::Severity::SHUTDOWN) {
      logger_->trace("Modbus operation cancelled due to shutdown: {}",
                     err.describe());
      connected_.store(false);
    }
  });

  meter_->setDeviceRetryCallback([this](int delay) {
    logger_->warn("Meter '{}' unavailable, retrying in {} {}...", cfg_.name,
                  delay, delay == 1 ? "second" : "seconds");
  });

  // NOTE: bus_->connect() is intentionally NOT called here. On a shared
  // bus, main() calls connect() once after every master has constructed
  // and registered its callbacks; connecting from each master would race
  // with later masters' registrations.

  // Start update loop thread. The loop body only runs once connected_
  // flips true, which the device-ready callback above does after
  // bus->connect() + validation.
  worker_ = std::thread(&FroniusMeter::runLoop, this);
}

FroniusMeter::~FroniusMeter() {
  // Detach from the bus before tearing down state that its callbacks
  // capture: unregisterDevice cancels any in-flight per-device retry
  // loop, and removeBusCallback synchronously waits for the bus thread
  // to finish any in-flight invocation of each callback. If the bus
  // outlives us (another master sharing it still holds a reference),
  // it continues to serve other devices uninterrupted.
  if (bus_) {
    if (meter_)
      bus_->unregisterDevice(meter_.get());
    for (auto id : busCallbackIds_)
      bus_->removeBusCallback(id);
  }
  busCallbackIds_.clear();

  // Stop the local update loop. The worker observes handler_.isRunning()
  // (cleared at signal time) and our cv_, so under graceful shutdown it
  // is usually already at its wait point and exits immediately on notify.
  connected_.store(false);
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();

  // Fire one final availability update so MQTT consumers see this meter
  // go offline. Done after the worker join so it can't race with an
  // in-flight updateValuesAndJson(). Safe because main destroys masters
  // before destroying MqttClient.
  if (availabilityCallback_)
    availabilityCallback_("disconnected");

  logger_->info("Meter '{}' disconnected", cfg_.name);
}

void FroniusMeter::runLoop() {
  while (handler_.isRunning()) {

    if (connected_.load()) {
      {
        // --- Device (once per connect; bool tells us whether to publish) ---
        auto deviceResult = updateDeviceAndJson();
        if (!deviceResult) {
          connected_.store(false);
        } else if (*deviceResult && deviceCallback_ && handler_.isRunning()) {
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
    cv_.wait_for(lock, std::chrono::seconds(fcfg_.updateInterval),
                 [this] { return !handler_.isRunning(); });
  }

  logger_->debug("Modbus master run loop stopped.");
}

std::string FroniusMeter::getJsonDump() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return jsonValues_.dump();
}

MeterTypes::Values FroniusMeter::getValues() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return values_;
}

ModbusDeviceConfig
FroniusMeter::makeDeviceConfig(const FroniusMeterConfig &cfg) {
  ModbusDeviceConfig devCfg;
  devCfg.slaveId = cfg.slaveId;
  devCfg.secTimeout = cfg.responseTimeout.sec;
  devCfg.usecTimeout = cfg.responseTimeout.usec;
  devCfg.reconnectDelay = cfg.reconnectDelay.min;
  devCfg.reconnectDelayMax = cfg.reconnectDelay.max;
  devCfg.exponential = cfg.reconnectDelay.exponential;
  return devCfg;
}

std::expected<void, ModbusError> FroniusMeter::updateValuesAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateValuesAndJson(): Shutdown in progress"));
  }

  auto regs = meter_->fetchMeterRegisters();
  if (!regs) {
    return std::unexpected(regs.error());
  }

  MeterTypes::Values values{};

  values.time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  try {
    values.activeEnergyImport = ModbusError::getOrThrow(
        meter_->getAcEnergyActive(FroniusTypes::EnergyDirection::IMPORT));
    values.activeEnergyExport = ModbusError::getOrThrow(
        meter_->getAcEnergyActive(FroniusTypes::EnergyDirection::EXPORT));

    values.activePower = ModbusError::getOrThrow(
        meter_->getAcPowerActive(FroniusTypes::Phase::TOTAL));

    values.apparentPower = ModbusError::getOrThrow(
        meter_->getAcPowerApparent(FroniusTypes::Phase::TOTAL));

    values.reactivePower = ModbusError::getOrThrow(
        meter_->getAcPowerReactive(FroniusTypes::Phase::TOTAL));

    values.powerFactor = ModbusError::getOrThrow(
        meter_->getAcPowerFactor(FroniusTypes::Phase::AVERAGE));

    values.phVoltage =
        ModbusError::getOrThrow(meter_->getAcVoltage(FroniusTypes::Phase::PHV));
    values.ppVoltage =
        ModbusError::getOrThrow(meter_->getAcVoltage(FroniusTypes::Phase::PPV));

    values.frequency = ModbusError::getOrThrow(meter_->getAcFrequency());

    // --- Per-phase (1 phase) ---
    values.phase1.activePower = ModbusError::getOrThrow(
        meter_->getAcPowerActive(FroniusTypes::Phase::A));
    values.phase1.apparentPower = ModbusError::getOrThrow(
        meter_->getAcPowerApparent(FroniusTypes::Phase::A));
    values.phase1.reactivePower = ModbusError::getOrThrow(
        meter_->getAcPowerReactive(FroniusTypes::Phase::A));
    values.phase1.powerFactor = ModbusError::getOrThrow(
        meter_->getAcPowerFactor(FroniusTypes::Phase::A));
    values.phase1.phVoltage =
        ModbusError::getOrThrow(meter_->getAcVoltage(FroniusTypes::Phase::A));
    values.phase1.ppVoltage =
        ModbusError::getOrThrow(meter_->getAcVoltage(FroniusTypes::Phase::AB));
    values.phase1.current =
        ModbusError::getOrThrow(meter_->getAcCurrent(FroniusTypes::Phase::A));

    // --- Per-phase (2 phases) ---
    if (meter_->getPhases() > 1) {
      values.phase2.activePower = ModbusError::getOrThrow(
          meter_->getAcPowerActive(FroniusTypes::Phase::B));
      values.phase2.apparentPower = ModbusError::getOrThrow(
          meter_->getAcPowerApparent(FroniusTypes::Phase::B));
      values.phase2.reactivePower = ModbusError::getOrThrow(
          meter_->getAcPowerReactive(FroniusTypes::Phase::B));
      values.phase2.powerFactor = ModbusError::getOrThrow(
          meter_->getAcPowerFactor(FroniusTypes::Phase::B));
      values.phase2.phVoltage =
          ModbusError::getOrThrow(meter_->getAcVoltage(FroniusTypes::Phase::B));
      values.phase2.ppVoltage = ModbusError::getOrThrow(
          meter_->getAcVoltage(FroniusTypes::Phase::BC));
      values.phase2.current =
          ModbusError::getOrThrow(meter_->getAcCurrent(FroniusTypes::Phase::B));
    }

    // --- Per-phase (3 phases) ---
    if (meter_->getPhases() > 2) {
      values.phase3.activePower = ModbusError::getOrThrow(
          meter_->getAcPowerActive(FroniusTypes::Phase::C));
      values.phase3.apparentPower = ModbusError::getOrThrow(
          meter_->getAcPowerApparent(FroniusTypes::Phase::C));
      values.phase3.reactivePower = ModbusError::getOrThrow(
          meter_->getAcPowerReactive(FroniusTypes::Phase::C));
      values.phase3.powerFactor = ModbusError::getOrThrow(
          meter_->getAcPowerFactor(FroniusTypes::Phase::C));
      values.phase3.phVoltage =
          ModbusError::getOrThrow(meter_->getAcVoltage(FroniusTypes::Phase::C));
      values.phase3.ppVoltage = ModbusError::getOrThrow(
          meter_->getAcVoltage(FroniusTypes::Phase::CA));
      values.phase3.current =
          ModbusError::getOrThrow(meter_->getAcCurrent(FroniusTypes::Phase::C));
    }

    // Proprietary register map exposes only per-phase current; sum them.
    if (meter_->getRegisterMap() == FroniusTypes::RegisterMap::PROPRIETARY) {
      values.current =
          values.phase1.current + values.phase2.current + values.phase3.current;
    } else {
      values.current = ModbusError::getOrThrow(
          meter_->getAcCurrent(FroniusTypes::Phase::TOTAL));
    }

    // Energy: each register map exposes only two of the three energy types
    // directly; derive the third from the other two.
    if (meter_->getRegisterMap() == FroniusTypes::RegisterMap::SUNSPEC) {
      values.apparentEnergyImport = ModbusError::getOrThrow(
          meter_->getAcEnergyApparent(FroniusTypes::EnergyDirection::IMPORT));
      values.apparentEnergyExport = ModbusError::getOrThrow(
          meter_->getAcEnergyApparent(FroniusTypes::EnergyDirection::EXPORT));

      // Derive reactive from apparent and active, signed by reactive power
      // direction.
      const double sign = values.reactivePower >= 0.0 ? 1.0 : -1.0;
      values.reactiveEnergyImport =
          sign *
          std::sqrt(values.apparentEnergyImport * values.apparentEnergyImport -
                    values.activeEnergyImport * values.activeEnergyImport);
      values.reactiveEnergyExport =
          sign *
          std::sqrt(values.apparentEnergyExport * values.apparentEnergyExport -
                    values.activeEnergyExport * values.activeEnergyExport);

    } else if (meter_->getRegisterMap() ==
               FroniusTypes::RegisterMap::PROPRIETARY) {
      values.reactiveEnergyImport = ModbusError::getOrThrow(
          meter_->getAcEnergyReactive(FroniusTypes::EnergyDirection::IMPORT));
      values.reactiveEnergyExport = ModbusError::getOrThrow(
          meter_->getAcEnergyReactive(FroniusTypes::EnergyDirection::EXPORT));

      // Derive apparent from active and reactive.
      values.apparentEnergyImport =
          std::hypot(values.activeEnergyImport, values.reactiveEnergyImport);
      values.apparentEnergyExport =
          std::hypot(values.activeEnergyExport, values.reactiveEnergyExport);
    }

  } catch (const ModbusError &err) {
    logger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  // Quantise to the output precision; every consumer (MQTT JSON, Postgres, the
  // debug log) then sees the same values.
  values.round();

  // ---- Build JSON ----
  json newJson;
  json phases = json::array();

  phases.push_back({
      {"id", 1},
      {"power_active", values.phase1.activePower},
      {"power_apparent", values.phase1.apparentPower},
      {"power_reactive", values.phase1.reactivePower},
      {"power_factor", values.phase1.powerFactor},
      {"voltage_ph", values.phase1.phVoltage},
      {"voltage_pp", values.phase1.ppVoltage},
      {"current", values.phase1.current},

  });

  if (meter_->getPhases() > 1) {
    phases.push_back({
        {"id", 2},
        {"power_active", values.phase2.activePower},
        {"power_apparent", values.phase2.apparentPower},
        {"power_reactive", values.phase2.reactivePower},
        {"power_factor", values.phase2.powerFactor},
        {"voltage_ph", values.phase2.phVoltage},
        {"voltage_pp", values.phase2.ppVoltage},
        {"current", values.phase2.current},

    });
  }

  if (meter_->getPhases() > 2) {
    phases.push_back({
        {"id", 3},
        {"power_active", values.phase3.activePower},
        {"power_apparent", values.phase3.apparentPower},
        {"power_reactive", values.phase3.reactivePower},
        {"power_factor", values.phase3.powerFactor},
        {"voltage_ph", values.phase3.phVoltage},
        {"voltage_pp", values.phase3.ppVoltage},
        {"current", values.phase3.current},
    });
  }

  newJson["time"] = values.time;
  newJson["energy_active_import"] =
      Utils::scaleToKilo(values.activeEnergyImport);
  newJson["energy_active_export"] =
      Utils::scaleToKilo(values.activeEnergyExport);
  newJson["energy_apparent_import"] =
      Utils::scaleToKilo(values.apparentEnergyImport);
  newJson["energy_apparent_export"] =
      Utils::scaleToKilo(values.apparentEnergyExport);
  newJson["energy_reactive_import"] =
      Utils::scaleToKilo(values.reactiveEnergyImport);
  newJson["energy_reactive_export"] =
      Utils::scaleToKilo(values.reactiveEnergyExport);
  newJson["power_active"] = values.activePower;
  newJson["power_apparent"] = values.apparentPower;
  newJson["power_reactive"] = values.reactivePower;
  newJson["power_factor"] = values.powerFactor;
  newJson["frequency"] = values.frequency;
  newJson["voltage_ph"] = values.phVoltage;
  newJson["voltage_pp"] = values.ppVoltage;
  newJson["current"] = values.current;
  newJson["phases"] = phases;

  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    values_ = std::move(values);
    jsonValues_ = std::move(newJson);
  }

  logger_->debug("{}", jsonValues_.dump());

  return {};
}

std::expected<bool, ModbusError> FroniusMeter::updateDeviceAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateDeviceAndJson(): Shutdown in progress"));
  }

  if (deviceGate_.hasValue())
    return false;

  MeterTypes::Device newDevice;

  try {
    newDevice.manufacturer = ModbusError::getOrThrow(meter_->getManufacturer());
    newDevice.model = ModbusError::getOrThrow(meter_->getDeviceModel());
    newDevice.serialNumber = ModbusError::getOrThrow(meter_->getSerialNumber());
    newDevice.fwVersion = ModbusError::getOrThrow(meter_->getFwVersion());
    newDevice.options = std::string(PROJECT_VERSION) + "-" + GIT_COMMIT_HASH;
    newDevice.registerModel =
        meter_->getUseFloatRegisters() ? "float" : "int+sf";
    newDevice.slaveID =
        ModbusError::getOrThrow(meter_->getModbusDeviceAddress());
  } catch (const ModbusError &err) {
    logger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  newDevice.id = meter_->getId();
  newDevice.phases = meter_->getPhases();

  // Compare received slave ID with configured
  if (fcfg_.slaveId != newDevice.slaveID) {
    logger_->warn("Slave ID mismatch: configured {}, received {}",
                  fcfg_.slaveId, newDevice.slaveID);
  }

  // ---- Build ordered JSON ----
  json newJson;

  newJson["manufacturer"] = newDevice.manufacturer;
  newJson["model"] = newDevice.model;
  newJson["serial_number"] = newDevice.serialNumber;
  newJson["firmware_version"] = newDevice.fwVersion;
  newJson["register_model"] = newDevice.registerModel;
  newJson["slave_id"] = newDevice.slaveID;
  newJson["meter_id"] = newDevice.id;
  newJson["phases"] = newDevice.phases;

  logger_->debug("{}", newJson.dump());

  // Record the identity as the baseline so the hasValue() guard short-circuits
  // the Modbus re-read on subsequent polls; this is the first (and only) read,
  // so the callback fires once.
  deviceGate_.changed(newDevice);

  // ---- Commit values ----
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    jsonDevice_ = std::move(newJson);
    device_ = std::move(newDevice);
  }

  return true;
}
