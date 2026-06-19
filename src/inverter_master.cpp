#include "inverter_master.h"
#include "config_yaml.h"
#include "inverter_types.h"
#include "utils.h"
#include <chrono>
#include <expected>
#include <fronius/fronius_bus.h>
#include <fronius/fronius_types.h>
#include <fronius/inverter.h>
#include <fronius/modbus_config.h>
#include <fronius/modbus_error.h>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::ordered_json;

InverterMaster::InverterMaster(const InverterConfig &cfg,
                               SignalHandler &signalHandler,
                               std::shared_ptr<FroniusBus> bus)
    : bus_(std::move(bus)), cfg_(cfg), handler_(signalHandler) {

  // Fixed class-based logger chain: inverter -> default. The device name
  // is no longer part of the logger name (it already appears in every
  // connect/disconnect message), so all inverters share one module.
  logger_ = spdlog::get("inverter");
  if (!logger_)
    logger_ = spdlog::default_logger();

  auto devCfg = makeDeviceConfig(cfg_);
  inverter_ = std::make_shared<Inverter>(bus_, devCfg);
  bus_->registerDevice(inverter_);

  // --- Bus-level callbacks ---

  busCallbackIds_.push_back(bus_->addBusConnectCallback([this] {
    if (cfg_.tcp) {
      auto remote = bus_->getRemoteEndpoint();
      logger_->info("Connected to inverter '{}' at {}:{}", cfg_.name, remote.ip,
                    remote.port);
    }
  }));

  busCallbackIds_.push_back(bus_->addBusDisconnectCallback([this](int delay) {
    logger_->warn("Inverter '{}' disconnected, trying to reconnect in {} {}...",
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

  inverter_->setDeviceReadyCallback([this](FroniusTypes::RegisterMap map) {
    logger_->debug("Inverter '{}' register map: {}", cfg_.name,
                   FroniusTypes::toString(map));
    connected_.store(true);

    if (availabilityCallback_)
      availabilityCallback_("connected");
  });

  inverter_->setDeviceUnavailableCallback([this] {
    connected_.store(false);

    if (availabilityCallback_)
      availabilityCallback_("disconnected");
  });

  inverter_->setDeviceErrorCallback([this](const ModbusError &err) {
    if (err.severity == ModbusError::Severity::FATAL) {
      logger_->error("FATAL Modbus error: {}", err.describe());
      handler_.shutdown();

    } else if (err.severity == ModbusError::Severity::TRANSIENT) {
      logger_->debug("Transient Modbus error: {}", err.describe());
      connected_.store(false);
      bus_->scheduleDeviceRetry(inverter_);

    } else if (err.severity == ModbusError::Severity::SHUTDOWN) {
      logger_->trace("Modbus operation cancelled due to shutdown: {}",
                     err.describe());
      connected_.store(false);
    }
  });

  inverter_->setDeviceRetryCallback([this](int delay) {
    logger_->warn("Inverter '{}' unavailable, retrying in {} {}...", cfg_.name,
                  delay, delay == 1 ? "second" : "seconds");
  });

  // NOTE: bus_->connect() is intentionally NOT called here. On a shared
  // bus, main() calls connect() once after every master has constructed
  // and registered its callbacks; connecting from each master would race
  // with later masters' registrations.

  // Start update loop thread. The loop body only runs once connected_
  // flips true, which the device-ready callback above does after
  // bus->connect() + validation.
  worker_ = std::thread(&InverterMaster::runLoop, this);
}

InverterMaster::~InverterMaster() {
  // Detach from the bus before tearing down state that its callbacks
  // capture: unregisterDevice cancels any in-flight per-device retry
  // loop, and removeBusCallback synchronously waits for the bus thread
  // to finish any in-flight invocation of each callback. If the bus
  // outlives us (another master sharing it still holds a reference),
  // it continues to serve other devices uninterrupted.
  if (bus_) {
    if (inverter_)
      bus_->unregisterDevice(inverter_.get());
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

  // Fire one final availability update so MQTT consumers see this inverter
  // go offline. Done after the worker join so it can't race with an
  // in-flight update*AndJson(). Safe because main destroys masters before
  // destroying MqttClient.
  if (availabilityCallback_)
    availabilityCallback_("disconnected");

  logger_->info("Inverter '{}' disconnected", cfg_.name);
}

void InverterMaster::runLoop() {
  while (handler_.isRunning()) {

    if (connected_.load()) {
      {
        // --- Device (once per connect; bool tells us whether to publish) ---
        auto deviceResult = updateDeviceAndJson();
        if (!deviceResult) {
          connected_.store(false);
        } else if (*deviceResult && deviceCallback_ && handler_.isRunning()) {
          std::string json;
          InverterTypes::Device dev;
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
          InverterTypes::Values vals;
          {
            std::lock_guard<std::mutex> lock(cbMutex_);
            json = jsonValues_.dump();
            vals = values_;
          }
          valueCallback_(std::move(json), std::move(vals));
        }

        // --- Events (de-duplicated by lastEventsHash_ inside the update) ---
        auto eventsResult = updateEventsAndJson();
        if (!eventsResult) {
          connected_.store(false);
        } else if (eventCallback_ && handler_.isRunning()) {
          std::string json;
          InverterTypes::Events evts;
          {
            std::lock_guard<std::mutex> lock(cbMutex_);
            json = jsonEvents_.dump();
            evts = events_;
          }
          eventCallback_(std::move(json), std::move(evts));
        }
      }
    }

    // --- Wait for next update interval ---
    std::unique_lock<std::mutex> lock(cbMutex_);
    cv_.wait_for(lock, std::chrono::seconds(cfg_.updateInterval),
                 [this] { return !handler_.isRunning(); });
  }

  logger_->debug("Modbus master run loop stopped.");
}

void InverterMaster::setValueCallback(
    std::function<void(std::string, InverterTypes::Values)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  valueCallback_ = std::move(cb);
}

void InverterMaster::setEventCallback(
    std::function<void(std::string, InverterTypes::Events)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  eventCallback_ = std::move(cb);
}

void InverterMaster::setDeviceCallback(
    std::function<void(std::string, InverterTypes::Device)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  deviceCallback_ = std::move(cb);
}

void InverterMaster::setAvailabilityCallback(
    std::function<void(std::string)> cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  availabilityCallback_ = std::move(cb);
}

std::string InverterMaster::getJsonDump() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return jsonValues_.dump();
}

InverterTypes::Values InverterMaster::getValues() const {
  std::lock_guard<std::mutex> lock(cbMutex_);
  return values_;
}

ModbusDeviceConfig InverterMaster::makeDeviceConfig(const InverterConfig &cfg) {
  ModbusDeviceConfig devCfg;
  devCfg.slaveId = cfg.slaveId;
  devCfg.secTimeout = cfg.responseTimeout.sec;
  devCfg.usecTimeout = cfg.responseTimeout.usec;
  devCfg.reconnectDelay = cfg.reconnectDelay.min;
  devCfg.reconnectDelayMax = cfg.reconnectDelay.max;
  devCfg.exponential = cfg.reconnectDelay.exponential;
  return devCfg;
}

std::expected<void, ModbusError> InverterMaster::updateValuesAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateValuesAndJson(): Shutdown in progress"));
  }

  auto regs = inverter_->fetchInverterRegisters();
  if (!regs) {
    return std::unexpected(regs.error());
  }

  InverterTypes::Values values{};

  values.time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  try {
    // AC values
    values.acEnergy = ModbusError::getOrThrow(inverter_->getAcEnergy());
    values.acPowerActive = ModbusError::getOrThrow(
        inverter_->getAcPower(FroniusTypes::Output::ACTIVE));
    values.acPowerApparent = ModbusError::getOrThrow(
        inverter_->getAcPower(FroniusTypes::Output::APPARENT));
    values.acPowerReactive = ModbusError::getOrThrow(
        inverter_->getAcPower(FroniusTypes::Output::REACTIVE));
    values.acPowerFactor = ModbusError::getOrThrow(
        inverter_->getAcPower(FroniusTypes::Output::FACTOR));

    // Phase 1
    values.phase1.acVoltage = ModbusError::getOrThrow(
        inverter_->getAcVoltage(FroniusTypes::Phase::A));
    values.phase1.acCurrent = ModbusError::getOrThrow(
        inverter_->getAcCurrent(FroniusTypes::Phase::A));

    // Phase 2
    if (inverter_->getPhases() > 1) {
      values.phase2.acVoltage = ModbusError::getOrThrow(
          inverter_->getAcVoltage(FroniusTypes::Phase::B));
      values.phase2.acCurrent = ModbusError::getOrThrow(
          inverter_->getAcCurrent(FroniusTypes::Phase::B));
    }

    // Phase 3
    if (inverter_->getPhases() > 2) {
      values.phase3.acVoltage = ModbusError::getOrThrow(
          inverter_->getAcVoltage(FroniusTypes::Phase::C));
      values.phase3.acCurrent = ModbusError::getOrThrow(
          inverter_->getAcCurrent(FroniusTypes::Phase::C));
    }

    values.acFrequency = ModbusError::getOrThrow(inverter_->getAcFrequency());

    // DC values
    values.dcPower = ModbusError::getOrThrow(
        inverter_->getDcPower(FroniusTypes::Input::TOTAL));
    values.input1.dcPower =
        ModbusError::getOrThrow(inverter_->getDcPower(FroniusTypes::Input::A));
    values.input1.dcVoltage = ModbusError::getOrThrow(
        inverter_->getDcVoltage(FroniusTypes::Input::A));
    values.input1.dcCurrent = ModbusError::getOrThrow(
        inverter_->getDcCurrent(FroniusTypes::Input::A));
    if (!inverter_->isHybrid())
      values.input1.dcEnergy = ModbusError::getOrThrow(
          inverter_->getDcEnergy(FroniusTypes::Input::A));

    if (inverter_->getInputs() == 2) {
      values.input2.dcPower = ModbusError::getOrThrow(
          inverter_->getDcPower(FroniusTypes::Input::B));
      values.input2.dcVoltage = ModbusError::getOrThrow(
          inverter_->getDcVoltage(FroniusTypes::Input::B));
      values.input2.dcCurrent = ModbusError::getOrThrow(
          inverter_->getDcCurrent(FroniusTypes::Input::B));
      if (!inverter_->isHybrid())
        values.input2.dcEnergy = ModbusError::getOrThrow(
            inverter_->getDcEnergy(FroniusTypes::Input::B));
    }

  } catch (const ModbusError &err) {
    logger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  if (std::abs(values.dcPower) > 1e-12) {
    values.efficiency = values.acPowerActive / values.dcPower * 100.0;
  } else {
    values.efficiency = 0.0;
  }

  // Quantise to the output precision; every consumer (MQTT JSON, Postgres, the
  // debug log) then sees the same values.
  values.round();

  // ---- Build JSON ----
  json newJson;

  newJson["time"] = values.time;
  newJson["ac_energy"] = Utils::scaleToKilo(values.acEnergy);

  // AC power metrics
  newJson["ac_power_active"] = values.acPowerActive;
  newJson["ac_power_apparent"] = values.acPowerApparent;
  newJson["ac_power_reactive"] = values.acPowerReactive;
  newJson["ac_power_factor"] = values.acPowerFactor;

  // ---- Phases ----
  json phases = json::array();
  std::array<const InverterTypes::Phase *, 3> phaseList = {
      &values.phase1, &values.phase2, &values.phase3};

  int phaseCount =
      std::clamp(inverter_->getPhases(), 1, static_cast<int>(phaseList.size()));

  for (int i = 0; i < phaseCount; ++i) {
    phases.push_back({
        {"id", i + 1},
        {"ac_voltage", phaseList[i]->acVoltage},
        {"ac_current", phaseList[i]->acCurrent},
    });
  }

  newJson["phases"] = std::move(phases);
  newJson["ac_frequency"] = values.acFrequency;
  newJson["dc_power"] = values.dcPower;
  newJson["efficiency"] = values.efficiency;

  // ---- DC Inputs ----
  json inputs = json::array();
  std::array<const InverterTypes::Input *, 2> inputList = {&values.input1,
                                                           &values.input2};

  int inputCount =
      std::clamp(inverter_->getInputs(), 1, static_cast<int>(inputList.size()));

  for (int i = 0; i < inputCount; ++i) {
    json input = {
        {"id", i + 1},
        {"dc_voltage", inputList[i]->dcVoltage},
        {"dc_current", inputList[i]->dcCurrent},
        {"dc_power", inputList[i]->dcPower},
    };

    if (!inverter_->isHybrid()) {
      input["dc_energy"] = Utils::scaleToKilo(inputList[i]->dcEnergy);
    }

    inputs.push_back(std::move(input));
  }
  newJson["inputs"] = std::move(inputs);

  logger_->debug("{}", newJson.dump());

  // ---- Commit values ----
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    jsonValues_ = std::move(newJson);
    values_ = std::move(values);
  }

  return {};
}

std::expected<void, ModbusError> InverterMaster::updateEventsAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateEventsAndJson(): Shutdown in progress"));
  }

  InverterTypes::Events newEvents;

  try {
    newEvents.activeCode = inverter_->getActiveStateCode();
    newEvents.state = ModbusError::getOrThrow(inverter_->getState());
    newEvents.events = ModbusError::getOrThrow(inverter_->getEvents());
  } catch (const ModbusError &err) {
    logger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  if (!newEvents.events.empty()) {
    std::ostringstream oss;
    for (size_t i = 0; i < newEvents.events.size(); ++i) {
      oss << newEvents.events[i];
      if (i + 1 < newEvents.events.size())
        oss << ", ";
    }
    const std::size_t currentHash = std::hash<std::string>{}(oss.str());

    if (!lastEventsHash_.has_value() || currentHash != *lastEventsHash_) {
      logger_->warn("Inverter reported events: [{}]", oss.str());
      lastEventsHash_ = currentHash;
    }
  } else {
    lastEventsHash_.reset();
  }

  // ---- Build JSON ----
  json newJson;

  newJson["active_code"] = newEvents.activeCode;
  newJson["state"] = newEvents.state;
  newJson["events"] = nlohmann::json::array();
  for (const auto &e : newEvents.events) {
    newJson["events"].push_back(e);
  }

  logger_->debug("{}", newJson.dump());

  // ---- Commit events ----
  {
    std::lock_guard<std::mutex> lock(cbMutex_);
    jsonEvents_ = std::move(newJson);
    events_ = std::move(newEvents);
  }

  return {};
}

std::expected<bool, ModbusError> InverterMaster::updateDeviceAndJson() {
  if (!handler_.isRunning()) {
    return std::unexpected(ModbusError::custom(
        EINTR, "updateDeviceAndJson(): Shutdown in progress"));
  }

  if (deviceGate_.hasValue())
    return false;

  InverterTypes::Device newDevice;

  try {
    newDevice.manufacturer =
        ModbusError::getOrThrow(inverter_->getManufacturer());
    newDevice.model = ModbusError::getOrThrow(inverter_->getDeviceModel());
    newDevice.serialNumber =
        ModbusError::getOrThrow(inverter_->getSerialNumber());
    newDevice.fwVersion = ModbusError::getOrThrow(inverter_->getFwVersion());
    newDevice.dataManagerVersion =
        ModbusError::getOrThrow(inverter_->getOptions());
    newDevice.registerModel =
        inverter_->getUseFloatRegisters() ? "float" : "int+sf";
    newDevice.slaveID =
        ModbusError::getOrThrow(inverter_->getModbusDeviceAddress());
    newDevice.acPowerApparent = ModbusError::getOrThrow(
        inverter_->getAcPowerRating(FroniusTypes::Output::APPARENT));
  } catch (const ModbusError &err) {
    logger_->warn("{}", err.message);
    return std::unexpected(err);
  }

  newDevice.id = inverter_->getId();
  newDevice.isHybrid = inverter_->isHybrid();
  newDevice.inputs = inverter_->getInputs();
  newDevice.phases = inverter_->getPhases();

  // Compare received slave ID with configured
  if (cfg_.slaveId != newDevice.slaveID) {
    logger_->warn("Slave ID mismatch: configured {}, received {}", cfg_.slaveId,
                  newDevice.slaveID);
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
  newJson["inverter_id"] = newDevice.id;
  newJson["hybrid"] = newDevice.isHybrid;
  newJson["mppt_tracker"] = newDevice.inputs;
  newJson["phases"] = newDevice.phases;
  newJson["power_rating"] = newDevice.acPowerApparent;

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
