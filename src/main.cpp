#include "config.h"
#include "config_yaml.h"
#include "easy_meter.h"
#include "fronius_meter.h"
#include "inverter_master.h"
#include "logger.h"
#include "meter_master.h"
#include "meter_slave.h"
#include "mqtt_client.h"
#include "postgres_client.h"
#include "privileges.h"
#include "signal_handler.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

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

  bool noMigrate = false;
  app.add_flag(
      "--no-migrate", noMigrate,
      "Verify the PostgreSQL schema for each device instead of "
      "creating or upgrading it (no effect without a postgres config)");

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

  // --no-migrate downgrades the per-device schema setup from create/upgrade to
  // verify-only. Harmless when postgres is not configured.
  if (cfg.postgres && noMigrate)
    cfg.postgres->autoMigrate = false;

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
  //   1. inverterMasters and meterMasters destruct first. A Modbus master's
  //      destructor (every inverter, and Fronius meters) calls
  //      bus_->unregisterDevice() to cancel any in-flight retry loop, then
  //      bus_->removeBusCallback() for each bus-level callback it registered;
  //      both must see a live FroniusBus so the bus can synchronously join its
  //      retry threads and wait out any in-flight callback invocation. An EBZ
  //      meter master holds no bus — it owns a serial fd and a read thread —
  //      so its destructor just joins that thread and closes the fd; it is
  //      unaffected by `buses` below but, like the others, must destruct
  //      before `mqtt` (see step 4).
  //   2. `buses` then drops the last shared_ptr<FroniusBus> for each bus.
  //      Each FroniusBus destructor joins its bus thread and cancels any
  //      pending transactions.
  //   3. meterSlaves destruct independently of the buses — they own their
  //      own listener thread and modbus context.
  //   4. mqtt destructs last among the I/O objects so that final
  //      availability publishes from master destructors land successfully.
  //      The optional PostgresClient sits with mqtt: it is a peer consumer the
  //      master callbacks feed, so it must outlive every master. It joins its
  //      own worker thread on destruction.
  std::unique_ptr<MqttClient> mqtt;
  std::unique_ptr<PostgresClient> postgres;
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

    // --- Start optional PostgreSQL consumer ---
    // Constructed only when a postgres section is present; otherwise the
    // bridge runs MQTT-only. The worker connects lazily, so a database that
    // is down at startup does not block anything here.
    if (cfg.postgres) {
      // cfg.deviceRegistry is the derived per-device roster (built by
      // loadConfig, like cfg.buses); the consumer writes it into
      // public.device_registry at startup.
      postgres = std::make_unique<PostgresClient>(
          *cfg.postgres, cfg.deviceRegistry, cfg.site, handler);
      mainLogger->info("PostgreSQL consumer enabled ({} mode)",
                       cfg.postgres->autoMigrate ? "migrate" : "verify");
    } else {
      mainLogger->info("PostgreSQL consumer disabled");
    }

    // --- Build bus registry + startup summary ---
    // cfg.buses is the derived, deduplicated set of buses (one per unique
    // RS-485 device path or TCP endpoint), synthesised by loadConfig() from
    // the inverter and meter sections — reconnect-delay already aggregated
    // across devices that share a bus. A single pass turns each entry into
    // exactly one FroniusBus (devices sharing a physical line share the
    // instance and serialise wire access through it) and logs one info line
    // describing it, so the wiring is visible in normal operation without
    // enabling debug. busSummaryLine() covers both transports; the EBZ
    // Easymeter reads a dedicated SML serial line, never joins a bus, and is
    // absent here. No hardware is opened until the connect() calls below.
    //
    // The 'bus' logger is dedicated so per-bus output can be silenced or
    // surfaced independently of the main and per-device modules; resolve it
    // once here for both the summary and the diagnostic callback below.
    auto busLogger = spdlog::get("bus");

    // Decide the libmodbus wire trace here, not in makeBusConfig(): the bus
    // registry is built during loadConfig(), before setupLogging() has
    // registered any logger, so the flag cannot be resolved at config-build
    // time. The hex dump is the most verbose bus diagnostic and sits one level
    // below the per-transaction 'bus' debug lines: `bus: debug` yields
    // queue/tx/rx diagnostics, `bus: trace` additionally turns on the raw
    // libmodbus wire dump. Only a dedicated 'bus' logger at trace level opts
    // in — a global trace level does not, matching the original behaviour.
    const bool busTrace =
        busLogger && busLogger->level() == spdlog::level::trace;

    if (!busLogger)
      busLogger = spdlog::default_logger();
    for (const auto &[key, info] : cfg.buses) {
      ModbusBusConfig busCfg = info.config;
      busCfg.debug = busTrace;
      buses.emplace(key, std::make_shared<FroniusBus>(busCfg));
      busLogger->info("{}", busSummaryLine(key, info));
    }

    // --- Register bus log callback ---
    // Per-bus diagnostic output (queue depth, slave switches, tx/rx
    // outcomes) goes to the same 'bus' logger at debug level. spdlog
    // defaults to info-level for unregistered loggers, so these lines are
    // filtered out by default; users opt in with `bus: debug` (or `trace`)
    // in the YAML logger.modules section.
    for (auto &[key, bus] : buses)
      bus->addBusLogCallback(
          [busLogger](const std::string &msg) { busLogger->debug("{}", msg); });

    // --- Start meter masters ---
    // Each meter master's value/device callbacks publish to MQTT under
    // <base>/meter/<name>/<suffix> and, if this meter has a slave block,
    // feed that slave's register map. The 'meter' class segment lets
    // downstream consumers subscribe to e.g. fronius-bridge/meter/+/values
    // to receive every meter without an explicit name allow-list.
    meterMasters.reserve(cfg.meters.size());
    for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
      const auto &mcfg = cfg.meters[i];

      // Construct the kind-appropriate master. Fronius meters attach to a
      // shared Modbus bus (looked up by the key their bus config produces);
      // the EBZ Easymeter owns its serial line and takes no bus. Both are
      // held through the MeterMaster base, so the callback wiring below is
      // identical regardless of kind.
      std::unique_ptr<MeterMaster> master;
      if (auto key = busKeyOf(mcfg)) {
        master = std::make_unique<FroniusMeter>(mcfg, handler, buses.at(*key));
      } else {
        master = std::make_unique<EasyMeter>(mcfg, handler);
      }

      // Raw pointer to the slave, lifetime-aligned with the master via
      // the index-aligned vectors above. nullptr when this meter has no
      // slave block.
      MeterSlave *slavePtr = meterSlaves[i].get();

      // Capture the topic base by value so the lambdas don't depend on
      // cfg outliving them (which it does, but explicit is better).
      const std::string topicBase = cfg.mqtt.topic + "/meter/" + mcfg.name;
      const std::string name = mcfg.name;

      master->setValueCallback(
          [&mqtt, &postgres, slavePtr, topicBase,
           name](std::string jsonDump, MeterTypes::Values values) {
            mqtt->publish(std::move(jsonDump), topicBase + "/values");
            // Copy to the postgres consumer (if enabled) before moving into the
            // slave register map.
            if (postgres)
              postgres->onMeter(name, values);
            if (slavePtr)
              slavePtr->updateValues(std::move(values));
          });

      master->setDeviceCallback(
          [&mqtt, &postgres, slavePtr, topicBase,
           name](std::string jsonDump, MeterTypes::Device device) {
            mqtt->publish(std::move(jsonDump), topicBase + "/device");
            if (postgres)
              postgres->onMeterDevice(name, device);
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
      auto inv = std::make_unique<InverterMaster>(icfg, handler,
                                                  buses.at(busKeyOf(icfg)));

      const std::string topicBase = cfg.mqtt.topic + "/inverter/" + icfg.name;
      const std::string name = icfg.name;

      inv->setValueCallback(
          [&mqtt, &postgres, topicBase, name](std::string jsonDump,
                                              InverterTypes::Values values) {
            mqtt->publish(std::move(jsonDump), topicBase + "/values");
            if (postgres)
              postgres->onInverter(name, std::move(values));
          });

      inv->setEventCallback(
          [&mqtt, topicBase](std::string jsonDump, InverterTypes::Events) {
            mqtt->publish(std::move(jsonDump), topicBase + "/events");
          });

      inv->setDeviceCallback(
          [&mqtt, &postgres, topicBase, name](std::string jsonDump,
                                              InverterTypes::Device device) {
            mqtt->publish(std::move(jsonDump), topicBase + "/device");
            if (postgres)
              postgres->onInverterDevice(name, std::move(device));
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
