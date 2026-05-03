#include "config.h"
#include "config_yaml.h"
#include "inverter_master.h"
#include "logger.h"
#include "meter_master.h"
#include "meter_slave.h"
#include "mqtt_client.h"
#include "privileges.h"
#include "signal_handler.h"
#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main(int argc, char *argv[]) {

  // --- Command line parsing ---
  CLI::App app{PROJECT_NAME " - " PROJECT_DESCRIPTION};

  // Version string
  std::string versionStr = std::string(PROJECT_NAME) + " v" + PROJECT_VERSION +
                           " (" + GIT_COMMIT_HASH + ")";

  app.set_version_flag("-V,--version", versionStr);

  std::string config;
  auto configOption = app.add_option("-c,--config", config, "Set config file")
                          ->required()
                          ->envname("FRONIUS_CONFIG")
                          ->check(CLI::ExistingFile);

  // Optional: prevent specifying both at the same time in help/UX
  configOption->excludes("--version");

  std::string runUser;
  std::string runGroup;
  app.add_option("-u,--user", runUser,
                 "Drop privileges to this user after startup")
      ->envname("FRONIUS_USER");
  app.add_option("-g,--group", runGroup,
                 "Drop privileges to this group after startup")
      ->envname("FRONIUS_GROUP");

  CLI11_PARSE(app, argc, argv);

  // --- Load config ---
  AppConfig cfg;
  try {
    cfg = loadConfig(config);
  } catch (const std::exception &ex) {
    std::cerr << "Error loading config: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }

  // --- Setup logging ---
  setupLogging(cfg.logger);
  std::shared_ptr<spdlog::logger> mainLogger = spdlog::get("main");
  if (!mainLogger)
    mainLogger = spdlog::default_logger();
  mainLogger->info("Starting {} with config '{}'", PROJECT_NAME, config);

  // Warn if --user/--group specified but not running as root
  if (!Privileges::isRoot() && !runUser.empty()) {
    mainLogger->error(
        "--user/--group options specified, but not running as root");
    return EXIT_FAILURE;
  }

  // Warn if running as root without privilege drop
  if (Privileges::isRoot() && runUser.empty()) {
    mainLogger->warn("Running as root without privilege dropping - "
                     "consider using --user/--group options");
  }

  // --- Setup signals and shutdown
  SignalHandler handler;

  // All objects are declared here so their lifetimes are identical
  std::unique_ptr<MeterSlave> slave;
  std::unique_ptr<MqttClient> mqtt;
  std::optional<MeterMaster> master;
  std::optional<InverterMaster> inverter;

  try {
    // --- Start meter slave ---
    if (cfg.meter && cfg.meter->slave) {
      slave = std::make_unique<MeterSlave>(*cfg.meter->slave, handler);
    } else {
      mainLogger->info("Meter slave disabled");
    }

    // --- Drop privileges after binding to privileged ports ---
    if (!runUser.empty() && Privileges::isRoot()) {
      Privileges::drop(runUser, runGroup);
      mainLogger->info("Dropped privileges to user '{}' group '{}'",
                       Privileges::getCurrentUser(),
                       Privileges::getCurrentGroup());
    }

    // --- Start MQTT client ---
    mqtt = std::make_unique<MqttClient>(cfg.mqtt, handler);

    // --- Build bus registry ---
    // Each unique device path or TCP endpoint gets exactly one FroniusBus.
    // Devices sharing a physical RS-485 port share the instance, serialising
    // all wire access through a single queue. validateConfig() has already
    // guaranteed that shared RTU devices have identical line parameters,
    // so the first config to claim a key is authoritative.
    std::map<std::string, std::shared_ptr<FroniusBus>> buses;

    auto busKey = [](const ModbusBusConfig &busCfg) -> std::string {
      return busCfg.isRtu()
                 ? busCfg.rtu().device
                 : busCfg.tcp().host + ":" + std::to_string(busCfg.tcp().port);
    };

    auto registerBus = [&](const ModbusBusConfig &busCfg) {
      auto key = busKey(busCfg);
      if (buses.find(key) == buses.end())
        buses.emplace(key, std::make_shared<FroniusBus>(busCfg));
    };

    if (cfg.meter)
      registerBus(MeterMaster::makeBusConfig(cfg.meter->master));
    if (cfg.inverter)
      registerBus(InverterMaster::makeBusConfig(*cfg.inverter));

    // --- Register bus log callback ---
    for (auto &[key, bus] : buses) {
      bus->addBusLogCallback([mainLogger](const std::string &msg) {
        mainLogger->debug("{}", msg);
      });
    }

    // --- Start meter master ---
    if (cfg.meter) {
      auto busCfg = MeterMaster::makeBusConfig(cfg.meter->master);
      master.emplace(cfg.meter->master, handler, buses.at(busKey(busCfg)));

      master->setValueCallback([&cfg, &mqtt,
                                &slave](std::string jsonDump,
                                        MeterTypes::Values values) {
        mqtt->publish(std::move(jsonDump), cfg.mqtt.topic + "/meter/values");
        if (slave)
          slave->updateValues(std::move(values));
      });
      master->setDeviceCallback([&cfg, &mqtt,
                                 &slave](std::string jsonDump,
                                         MeterTypes::Device device) {
        mqtt->publish(std::move(jsonDump), cfg.mqtt.topic + "/meter/device");
        if (slave)
          slave->updateDevice(std::move(device));
      });
      master->setAvailabilityCallback([&mqtt, &cfg](std::string availability) {
        mqtt->publish(std::move(availability),
                      cfg.mqtt.topic + "/meter/availability");
      });
    } else {
      mainLogger->info("Meter disabled");
    }

    // --- Start inverter ---
    if (cfg.inverter) {
      auto busCfg = InverterMaster::makeBusConfig(*cfg.inverter);
      inverter.emplace(*cfg.inverter, handler, buses.at(busKey(busCfg)));

      inverter->setValueCallback([&mqtt, &cfg](std::string jsonDump) {
        mqtt->publish(std::move(jsonDump), cfg.mqtt.topic + "/inverter/values");
      });
      inverter->setEventCallback([&mqtt, &cfg](std::string jsonDump) {
        mqtt->publish(std::move(jsonDump), cfg.mqtt.topic + "/inverter/events");
      });
      inverter->setDeviceCallback([&mqtt, &cfg](std::string jsonDump) {
        mqtt->publish(std::move(jsonDump), cfg.mqtt.topic + "/inverter/device");
      });
      inverter->setAvailabilityCallback([&mqtt, &cfg](
                                            const std::string &availability) {
        mqtt->publish(availability, cfg.mqtt.topic + "/inverter/availability");
      });
    } else {
      mainLogger->info("Inverter disabled");
    }
  } catch (const std::exception &ex) {
    mainLogger->error("Startup failed: {}", ex.what());
    handler.shutdown();
    return EXIT_FAILURE;
  }

  // --- Wait for shutdown signal ---
  handler.wait();

  // --- Shutdown ---
  mainLogger->info("Shutting down due to signal {} ({})", handler.signalName(),
                   handler.signal());

  return EXIT_SUCCESS;
}