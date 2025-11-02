#include "config.h"
#include "config_yaml.h"
#include "logger.h"
#include "modbus_master.h"
#include "mqtt_client.h"
#include "signal_handler.h"
#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main(int argc, char *argv[]) {

  // --- Command line parsing ---
  CLI::App app{PROJECT_NAME " - Lightweight Modbus-to-MQTT bridge"};

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

  CLI11_PARSE(app, argc, argv);

  // --- Load config ---
  Config cfg;
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
  mainLogger->info("Starting fronius-ng with config '{}'", config);

  // --- Setup signals and shutdown
  SignalHandler handler;

  // --- Start ModbusMaster
  ModbusMaster master(cfg.modbus, handler);

  // --- Start MQTT consumer ---
  MqttClient mqtt(cfg.mqtt, handler);
  master.setValueCallback([&mqtt, &cfg](const std::string &jsonDump) {
    mqtt.publish(jsonDump, cfg.mqtt.topic + "/values");
  });
  master.setEventCallback([&mqtt, &cfg](const std::string &jsonDump) {
    mqtt.publish(jsonDump, cfg.mqtt.topic + "/events");
  });
  master.setDeviceCallback([&mqtt, &cfg](const std::string &jsonDump) {
    mqtt.publish(jsonDump, cfg.mqtt.topic + "/device");
  });

  // --- Wait for shutdown signal ---
  handler.wait();

  // --- Shutdown ---
  mainLogger->info("Shutting down due to signal {} ({})", handler.signalName(),
                   handler.signal());

  return EXIT_SUCCESS;
}