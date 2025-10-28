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
  CLI::App app{"fronius-ng - Talk to Fronius inverters"};

  bool version = false;
  std::string config;
  app.add_flag("-V,--version", version, "Show build info");
  app.add_option("-c,--config", config, "Set config file")->required();

  CLI11_PARSE(app, argc, argv);

  if (version) {
    std::cout << "Version: " << PROJECT_VERSION << "\n";
    std::cout << "Git commit hash: " << GIT_COMMIT_HASH << "\n";
    std::cout << "Commit date: " << GIT_COMMIT_DATE << "\n";
    return EXIT_SUCCESS;
  }

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
  master.setUpdateCallback(
      [&mqtt](const std::string &jsonDump) { mqtt.publishValues(jsonDump); });
  master.setEventCallback([&mqtt, &cfg](const std::string &jsonDump) {
    mqtt.publish(jsonDump, cfg.mqtt.topic + "/events");
  });

  // --- Wait for shutdown signal ---
  handler.wait();

  // --- Shutdown ---
  mainLogger->info("Shutting down due to signal {} ({})", handler.signalName(),
                   handler.signal());

  return EXIT_SUCCESS;
}