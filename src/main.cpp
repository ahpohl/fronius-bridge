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
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

using json = nlohmann::json;

namespace {

// Stable string identity for a Modbus bus. Two devices that produce the same
// key share an underlying FroniusBus instance and serialise wire access.
std::string busKey(const ModbusBusConfig &busCfg) {
  return busCfg.isRtu()
             ? busCfg.rtu().device
             : busCfg.tcp().host + ":" + std::to_string(busCfg.tcp().port);
}

// Fold a device's reconnect-delay parameters into the bus-level aggregate.
// On a shared bus we cannot honour each device's reconnect policy
// individually — the bus has a single reconnect schedule — so we pick the
// most-responsive interpretation:
//   - min: smallest min across all devices (fastest first retry)
//   - max: smallest max across all devices (cap backoff at the
//          most-impatient device's tolerance)
//   - exponential: true if any device wants exponential backoff
//
// If every device configures the same values, this aggregation is a no-op.
void mergeReconnectDelay(ModbusBusConfig &dst, const ModbusBusConfig &src) {
  dst.reconnectDelay = std::min(dst.reconnectDelay, src.reconnectDelay);
  dst.reconnectDelayMax = std::min(dst.reconnectDelayMax, src.reconnectDelayMax);
  dst.exponential = dst.exponential || src.exponential;
}

} // namespace

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

  // All objects are declared here so their lifetimes are identical.
  // Parallel vectors cfg.meters / meterMasters / meterSlaves are kept
  // index-aligned: meter i, its master, and its slave (if any) all line up.
  // meterSlaves[i] is nullptr when meter i has no `slave` block.
  //
  // Declaration order matters here. Destruction runs in reverse, so:
  //   1. inverterMasters and meterMasters destruct first. Each master's
  //      destructor calls bus_->unregisterDevice() to cancel any in-flight
  //      retry loop, then bus_->removeBusCallback() for each bus-level
  //      callback the master registered. Both must see a live FroniusBus
  //      so the bus can synchronously join its retry threads and wait out
  //      any in-flight callback invocation.
  //   2. `buses` then drops the last shared_ptr<FroniusBus> for each bus.
  //      Each FroniusBus destructor joins its bus thread and cancels any
  //      pending transactions.
  //   3. meterSlaves destruct independently of the buses — they own their
  //      own listener thread and modbus context.
  //   4. mqtt destructs last among the I/O objects so that final
  //      availability publishes from master destructors land successfully.
  std::unique_ptr<MqttClient> mqtt;
  std::vector<std::unique_ptr<MeterSlave>> meterSlaves;
  std::map<std::string, std::shared_ptr<FroniusBus>> buses;
  std::vector<std::unique_ptr<MeterMaster>> meterMasters;
  std::vector<std::unique_ptr<InverterMaster>> inverterMasters;

  try {
    // --- Start meter slaves ---
    // Slaves come first because they bind (potentially privileged) TCP
    // ports and need to do so before we drop root. The vector is index-
    // aligned with cfg.meters: meterSlaves[i] corresponds to cfg.meters[i]
    // and is nullptr when that meter has no `slave` block.
    meterSlaves.reserve(cfg.meters.size());
    for (const auto &m : cfg.meters) {
      if (m.slave) {
        meterSlaves.push_back(
            std::make_unique<MeterSlave>(*m.slave, m.name, handler));
        mainLogger->info("Meter slave '{}' enabled", m.name);
      } else {
        meterSlaves.push_back(nullptr);
      }
    }
    if (std::all_of(meterSlaves.begin(), meterSlaves.end(),
                    [](const auto &p) { return !p; })) {
      mainLogger->info("No meter slaves configured");
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
    // guaranteed that shared RTU devices have identical line parameters.
    //
    // Reconnect-delay parameters can differ between devices on the same
    // bus. We aggregate them via mergeReconnectDelay() before constructing
    // any FroniusBus, so each bus sees a single deterministic policy
    // rather than whichever device happened to register first.
    {
      std::map<std::string, ModbusBusConfig> aggregated;
      auto fold = [&](const ModbusBusConfig &busCfg) {
        auto key = busKey(busCfg);
        auto [it, inserted] = aggregated.try_emplace(key, busCfg);
        if (!inserted)
          mergeReconnectDelay(it->second, busCfg);
      };
      for (const auto &m : cfg.meters)
        fold(MeterMaster::makeBusConfig(m));
      for (const auto &i : cfg.inverters)
        fold(InverterMaster::makeBusConfig(i));

      for (auto &[key, busCfg] : aggregated)
        buses.emplace(key, std::make_shared<FroniusBus>(busCfg));
    }

    // --- Register bus log callback ---
    // Per-bus diagnostic output (queue depth, slave switches, tx/rx
    // outcomes) goes to its own 'bus' logger so it can be silenced or
    // surfaced independently of the main and per-device modules. spdlog
    // defaults to info-level for unregistered loggers, so by default these
    // debug-level lines are filtered out; users opt in with `bus: debug`
    // (or `trace`) in the YAML logger.modules section.
    auto busLogger = spdlog::get("bus");
    if (!busLogger)
      busLogger = spdlog::default_logger();
    for (auto &[key, bus] : buses) {
      bus->addBusLogCallback([busLogger](const std::string &msg) {
        busLogger->debug("{}", msg);
      });
    }

    // --- Start meter masters ---
    // Each meter master's value/device callbacks publish to MQTT under
    // <base>/meter/<name>/<suffix> and, if this meter has a slave block,
    // feed that slave's register map. The 'meter' class segment lets
    // downstream consumers subscribe to e.g. fronius-bridge/meter/+/values
    // to receive every meter without an explicit name allow-list.
    meterMasters.reserve(cfg.meters.size());
    for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
      const auto &mcfg = cfg.meters[i];
      auto busCfg = MeterMaster::makeBusConfig(mcfg);
      auto master = std::make_unique<MeterMaster>(mcfg, handler,
                                                  buses.at(busKey(busCfg)));

      // Raw pointer to the slave, lifetime-aligned with the master via
      // the index-aligned vectors above. nullptr when this meter has no
      // slave block.
      MeterSlave *slavePtr = meterSlaves[i].get();

      // Capture the topic base by value so the lambdas don't depend on
      // cfg outliving them (which it does, but explicit is better).
      const std::string topicBase =
          cfg.mqtt.topic + "/meter/" + mcfg.name;

      master->setValueCallback(
          [&mqtt, slavePtr, topicBase](std::string jsonDump,
                                       MeterTypes::Values values) {
            mqtt->publish(std::move(jsonDump), topicBase + "/values");
            if (slavePtr)
              slavePtr->updateValues(std::move(values));
          });

      master->setDeviceCallback(
          [&mqtt, slavePtr, topicBase](std::string jsonDump,
                                       MeterTypes::Device device) {
            mqtt->publish(std::move(jsonDump), topicBase + "/device");
            if (slavePtr)
              slavePtr->updateDevice(std::move(device));
          });

      master->setAvailabilityCallback(
          [&mqtt, topicBase](std::string availability) {
            mqtt->publish(std::move(availability), topicBase + "/availability");
          });

      meterMasters.push_back(std::move(master));
    }
    if (cfg.meters.empty())
      mainLogger->info("No meters configured");

    // --- Start inverter masters ---
    inverterMasters.reserve(cfg.inverters.size());
    for (const auto &icfg : cfg.inverters) {
      auto busCfg = InverterMaster::makeBusConfig(icfg);
      auto inv = std::make_unique<InverterMaster>(icfg, handler,
                                                  buses.at(busKey(busCfg)));

      const std::string topicBase =
          cfg.mqtt.topic + "/inverter/" + icfg.name;

      inv->setValueCallback(
          [&mqtt, topicBase](std::string jsonDump, InverterTypes::Values) {
            mqtt->publish(std::move(jsonDump), topicBase + "/values");
          });

      inv->setEventCallback(
          [&mqtt, topicBase](std::string jsonDump, InverterTypes::Events) {
            mqtt->publish(std::move(jsonDump), topicBase + "/events");
          });

      inv->setDeviceCallback(
          [&mqtt, topicBase](std::string jsonDump, InverterTypes::Device) {
            mqtt->publish(std::move(jsonDump), topicBase + "/device");
          });

      inv->setAvailabilityCallback(
          [&mqtt, topicBase](const std::string &availability) {
            mqtt->publish(availability, topicBase + "/availability");
          });

      inverterMasters.push_back(std::move(inv));
    }
    if (cfg.inverters.empty())
      mainLogger->info("No inverters configured");

    // --- Start the buses ---
    // bus->connect() is called only now, after every master sharing each
    // bus has constructed and registered its callbacks. Calling it earlier
    // would race: the bus thread could fire onBusConnect_ before later
    // masters added their callbacks. The bus's running_.exchange(true)
    // guard makes this a single-shot per bus.
    for (auto &[key, bus] : buses)
      bus->connect();

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
