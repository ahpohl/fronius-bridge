#include "config_yaml.h"
#include <format>
#include <map>
#include <regex>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// Serial conversion helpers
// ---------------------------------------------------------------------------

char parityToChar(Parity parity) {
  switch (parity) {
  case Parity::Even:
    return 'E';
  case Parity::Odd:
    return 'O';
  default:
    return 'N';
  }
}

speed_t baudToSpeed(int baud) {
  switch (baud) {
  case 1200:
    return B1200;
  case 2400:
    return B2400;
  case 4800:
    return B4800;
  case 9600:
    return B9600;
  case 19200:
    return B19200;
  case 38400:
    return B38400;
  case 57600:
    return B57600;
  case 115200:
    return B115200;
  case 230400:
    return B230400;
  case 460800:
    return B460800;
  default:
    throw std::invalid_argument("Unsupported baud rate: " +
                                std::to_string(baud));
  }
}

tcflag_t dataBitsToFlag(int dataBits) {
  switch (dataBits) {
  case 5:
    return CS5;
  case 6:
    return CS6;
  case 7:
    return CS7;
  case 8:
    return CS8;
  default:
    throw std::invalid_argument("data_bits must be 5, 6, 7 or 8");
  }
}

void applyParity(termios &tty, Parity parity) {
  switch (parity) {
  case Parity::None:
    tty.c_cflag &= ~PARENB;
    break;
  case Parity::Even:
    tty.c_cflag |= PARENB;
    tty.c_cflag &= ~PARODD;
    break;
  case Parity::Odd:
    tty.c_cflag |= PARENB;
    tty.c_cflag |= PARODD;
    break;
  }
}

static Parity parseParity(const std::string &val) {
  if (val == "none")
    return Parity::None;
  if (val == "even")
    return Parity::Even;
  if (val == "odd")
    return Parity::Odd;
  throw std::invalid_argument("parity must be one of: none, even, odd");
}

// ---------------------------------------------------------------------------
// Internal parse helpers
// ---------------------------------------------------------------------------

static std::optional<ModbusTcpClientConfig>
parseTcpClient(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  if (!node["host"])
    throw std::runtime_error(".tcp.host is required");

  ModbusTcpClientConfig tcp;
  tcp.host = node["host"].as<std::string>();
  tcp.port = node["port"].as<int>(502);

  if (tcp.port <= 0 || tcp.port > 65535)
    throw std::invalid_argument(".tcp.port must be in range 1-65535");

  return tcp;
}

static std::optional<ModbusTcpServerConfig>
parseTcpServer(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  ModbusTcpServerConfig tcp;
  tcp.listen = node["listen"].as<std::string>("0.0.0.0");
  tcp.port = node["port"].as<int>(502);

  if (tcp.port <= 0 || tcp.port > 65535)
    throw std::invalid_argument(".tcp.port must be in range 1-65535");

  return tcp;
}

static std::optional<ModbusRtuConfig> parseRtu(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  if (!node["device"])
    throw std::runtime_error(".rtu.device is required");

  ModbusRtuConfig rtu;
  rtu.device = node["device"].as<std::string>();
  rtu.baud = node["baud"].as<int>(9600);
  rtu.dataBits = node["data_bits"].as<int>(8);
  rtu.stopBits = node["stop_bits"].as<int>(1);
  rtu.parity = node["parity"] ? parseParity(node["parity"].as<std::string>())
                              : Parity::None;

  if (rtu.baud <= 0)
    throw std::invalid_argument(".rtu.baud must be positive");
  if (rtu.dataBits < 5 || rtu.dataBits > 8)
    throw std::invalid_argument(".rtu.data_bits must be 5-8");
  if (rtu.stopBits != 1 && rtu.stopBits != 2)
    throw std::invalid_argument(".rtu.stop_bits must be 1 or 2");

  return rtu;
}

static ReconnectDelayConfig parseReconnectDelay(const YAML::Node &node) {
  ReconnectDelayConfig cfg;
  if (!node)
    return cfg;

  cfg.min = node["min"].as<int>(5);
  cfg.max = node["max"].as<int>(320);
  cfg.exponential = node["exponential"].as<bool>(true);

  if (cfg.min <= 0)
    throw std::invalid_argument(".reconnect_delay.min must be positive");
  if (cfg.max <= 0)
    throw std::invalid_argument(".reconnect_delay.max must be positive");
  if (cfg.min >= cfg.max)
    throw std::invalid_argument(".reconnect_delay.min must be less than max");

  return cfg;
}

static ResponseTimeoutConfig parseResponseTimeout(const YAML::Node &node) {
  ResponseTimeoutConfig cfg;
  if (!node)
    return cfg;

  cfg.sec = node["sec"].as<int>(5);
  cfg.usec = node["usec"].as<int>(0);

  if (cfg.sec < 0)
    throw std::invalid_argument(".response_timeout.sec must be non-negative");
  if (cfg.usec < 0 || cfg.usec >= 1000000)
    throw std::invalid_argument(
        ".response_timeout.usec must be in range 0-999999");
  if (cfg.sec == 0 && cfg.usec == 0)
    throw std::invalid_argument(".response_timeout must be greater than zero");

  return cfg;
}

// ---------------------------------------------------------------------------
// Name validation
//
// Device names are used as MQTT topic segments (between the configured base
// topic and the per-device suffixes such as `/values`, `/availability`) and
// as logger names: each device's logger lookup chain tries `<name>.master`
// or `<name>.slave` first, then `<name>` (so users can configure both
// master and slave for a device with one YAML key), then the default.
//
// Restrict to a safe character set so names cannot break MQTT topics
// (no '+', '#', '/') or surprise downstream consumers.
//
// Reserved literals fall into two groups:
//   - MQTT-topic ambiguity: `meter`, `inverter` would produce a topic
//     like `<base>/meter/meter/values` or `<base>/inverter/inverter/values`
//     which is parseable but visually confusing for anyone reading the
//     topic stream.
//   - Logger-name collision: `master`, `slave`, `main`, `mqtt`, `bus`
//     either shadow a built-in logger module (`main`, `mqtt`, `bus`) or
//     produce a confusing combined logger name (a device named `master`
//     would resolve to logger `master.master`).
//
// Throws messages prefixed with `:` so the calling parser prepends its own
// section label (`inverters[0]`, `meters[1]`, …) via the surrounding
// try/catch.
// ---------------------------------------------------------------------------

static void validateName(const std::string &name) {
  if (name.empty())
    throw std::invalid_argument(": .name must not be empty");
  if (name.size() > 32)
    throw std::invalid_argument(": .name must be 32 characters or fewer");

  static const std::regex pattern("[A-Za-z0-9_-]+");
  if (!std::regex_match(name, pattern))
    throw std::invalid_argument(
        ": .name must contain only [A-Za-z0-9_-] characters");

  // The two-tier reserved-name check below mirrors the comment above.
  if (name == "meter" || name == "inverter")
    throw std::invalid_argument(
        std::string(": .name '") + name +
        "' is reserved (would produce a visually ambiguous topic of "
        "the form <base>/" +
        name + "/" + name + "/...)");

  if (name == "master" || name == "slave" || name == "main" ||
      name == "mqtt" || name == "bus")
    throw std::invalid_argument(
        std::string(": .name '") + name +
        "' is reserved (would conflict with a built-in logger module or "
        "produce an ambiguous combined logger name)");
}

// Shared logic for any Modbus master-role section (inverter or meter).
// Fills the fields that InverterConfig and MeterConfig have in common;
// the caller is responsible for `name` and any role-specific extras
// (e.g. MeterConfig::slave).
template <typename T> static T parseModbusMaster(const YAML::Node &node) {
  T cfg;

  cfg.tcp = parseTcpClient(node["tcp"]);
  cfg.rtu = parseRtu(node["rtu"]);

  if (cfg.tcp.has_value() == cfg.rtu.has_value())
    throw std::runtime_error(": exactly one of tcp or rtu must be specified");

  cfg.slaveId = node["unit_id"].as<int>(1);
  cfg.updateInterval = node["update_interval"].as<int>(4);
  cfg.responseTimeout = parseResponseTimeout(node["response_timeout"]);
  cfg.reconnectDelay = parseReconnectDelay(node["reconnect_delay"]);

  if (cfg.slaveId < 1 || cfg.slaveId > 247)
    throw std::invalid_argument(".unit_id must be in range 1-247");
  if (cfg.updateInterval <= 0)
    throw std::invalid_argument(".update_interval must be positive");

  return cfg;
}

// ---------------------------------------------------------------------------
// Section parsers
// ---------------------------------------------------------------------------

static std::vector<InverterConfig> parseInverters(const YAML::Node &node) {
  std::vector<InverterConfig> result;
  if (!node)
    return result;

  if (!node.IsSequence())
    throw std::runtime_error("inverters must be a sequence");

  result.reserve(node.size());
  for (std::size_t i = 0; i < node.size(); ++i) {
    const auto prefix = std::format("inverters[{}]", i);
    try {
      auto cfg = parseModbusMaster<InverterConfig>(node[i]);
      cfg.name = node[i]["name"].as<std::string>("");
      validateName(cfg.name);
      result.push_back(std::move(cfg));
    } catch (const std::exception &e) {
      throw std::runtime_error(prefix + e.what());
    }
  }
  return result;
}

static std::optional<MeterSlaveConfig> parseMeterSlave(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  MeterSlaveConfig cfg;

  cfg.tcp = parseTcpServer(node["tcp"]);
  cfg.rtu = parseRtu(node["rtu"]);

  if (cfg.tcp.has_value() == cfg.rtu.has_value())
    throw std::runtime_error(": exactly one of tcp or rtu must be specified");

  cfg.slaveId = node["unit_id"].as<int>(1);
  cfg.requestTimeout = node["request_timeout"].as<int>(5);
  cfg.idleTimeout = node["idle_timeout"].as<int>(60);
  cfg.useFloatModel = node["use_float_model"].as<bool>(false);

  if (cfg.slaveId < 1 || cfg.slaveId > 247)
    throw std::invalid_argument(".unit_id must be in range 1-247");
  if (cfg.requestTimeout <= 0)
    throw std::invalid_argument(".request_timeout must be positive");
  if (cfg.idleTimeout < cfg.requestTimeout)
    throw std::invalid_argument(".idle_timeout must be >= request_timeout");

  return cfg;
}

static std::vector<MeterConfig> parseMeters(const YAML::Node &node) {
  std::vector<MeterConfig> result;
  if (!node)
    return result;

  if (!node.IsSequence())
    throw std::runtime_error("meters must be a sequence");

  result.reserve(node.size());
  for (std::size_t i = 0; i < node.size(); ++i) {
    const auto prefix = std::format("meters[{}]", i);
    try {
      auto cfg = parseModbusMaster<MeterConfig>(node[i]);
      cfg.name = node[i]["name"].as<std::string>("");
      validateName(cfg.name);

      try {
        cfg.slave = parseMeterSlave(node[i]["slave"]);
      } catch (const std::exception &e) {
        throw std::runtime_error(std::string(".slave") + e.what());
      }

      result.push_back(std::move(cfg));
    } catch (const std::exception &e) {
      throw std::runtime_error(prefix + e.what());
    }
  }
  return result;
}

static MqttConfig parseMqtt(const YAML::Node &node) {
  if (!node)
    throw std::runtime_error("Missing mqtt section in config");

  MqttConfig cfg;
  cfg.broker = node["broker"].as<std::string>("localhost");
  cfg.port = node["port"].as<int>(1883);
  cfg.topic = node["topic"].as<std::string>("fronius-bridge");
  cfg.queueSize = node["queue_size"].as<size_t>(100);

  if (node["user"])
    cfg.user = node["user"].as<std::string>();
  if (node["password"])
    cfg.password = node["password"].as<std::string>();

  cfg.reconnectDelay = parseReconnectDelay(node["reconnect_delay"]);

  if (cfg.port <= 0 || cfg.port > 65535)
    throw std::invalid_argument("mqtt.port must be in range 1-65535");
  if (cfg.queueSize == 0)
    throw std::invalid_argument("mqtt.queue_size must be greater than zero");

  return cfg;
}

static spdlog::level::level_enum parseLogLevel(const std::string &s) {
  if (s == "off")
    return spdlog::level::off;
  if (s == "error")
    return spdlog::level::err;
  if (s == "warn")
    return spdlog::level::warn;
  if (s == "info")
    return spdlog::level::info;
  if (s == "debug")
    return spdlog::level::debug;
  if (s == "trace")
    return spdlog::level::trace;
  throw std::invalid_argument("Unknown log level " + s);
}

static LoggerConfig parseLogger(const YAML::Node &node) {
  LoggerConfig cfg;
  if (!node)
    return cfg;

  if (node["level"])
    cfg.globalLevel = parseLogLevel(node["level"].as<std::string>());

  if (node["modules"]) {
    for (auto it : node["modules"]) {
      std::string key = it.first.as<std::string>();
      if (it.second.IsMap()) {
        for (auto sub : it.second) {
          std::string subkey = sub.first.as<std::string>();
          cfg.moduleLevels[key + "." + subkey] =
              parseLogLevel(sub.second.as<std::string>());
        }
      } else {
        cfg.moduleLevels[key] = parseLogLevel(it.second.as<std::string>());
      }
    }
  }

  return cfg;
}

// ---------------------------------------------------------------------------
// Cross section validation
// ---------------------------------------------------------------------------

namespace {

// Stable string identity for a bus (RTU device path, or TCP host:port).
// Kept in sync with the lambda in main.cpp; if/when that is factored out
// of main.cpp, this duplication goes away.
std::string busKeyTcp(const ModbusTcpClientConfig &tcp) {
  return tcp.host + ":" + std::to_string(tcp.port);
}

std::string busKeyRtu(const ModbusRtuConfig &rtu) { return rtu.device; }

// A uniform view of a master-role device (inverter or meter) for the
// purposes of cross-section validation. Pointers reference fields inside
// the AppConfig that the surrounding validateConfig() call owns, so
// DeviceRef must not outlive that call.
struct DeviceRef {
  std::string_view kind; // "inverter" or "meter"
  std::size_t index;
  std::string_view name;
  const ModbusTcpClientConfig *tcp;
  const ModbusRtuConfig *rtu;
  int slaveId;
};

std::string describe(const DeviceRef &d) {
  return std::format("{}[{}] ('{}')", d.kind, d.index, d.name);
}

std::string busKey(const DeviceRef &d) {
  return d.rtu ? busKeyRtu(*d.rtu) : busKeyTcp(*d.tcp);
}

} // namespace

static void validateConfig(const AppConfig &cfg) {
  // --- Collect master-role devices in a uniform form ---
  std::vector<DeviceRef> masters;
  masters.reserve(cfg.inverters.size() + cfg.meters.size());

  for (std::size_t i = 0; i < cfg.inverters.size(); ++i) {
    const auto &c = cfg.inverters[i];
    masters.push_back({"inverter", i, c.name, c.tcp ? &*c.tcp : nullptr,
                       c.rtu ? &*c.rtu : nullptr, c.slaveId});
  }
  for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
    const auto &c = cfg.meters[i];
    masters.push_back({"meter", i, c.name, c.tcp ? &*c.tcp : nullptr,
                       c.rtu ? &*c.rtu : nullptr, c.slaveId});
  }

  // --- Name uniqueness across all master-role devices ---
  // Names are used as MQTT topic segments and as logger suffixes;
  // collisions between inverter and meter names would be ambiguous, so the
  // scope is global rather than per-kind.
  {
    std::map<std::string, DeviceRef> byName;
    for (const auto &d : masters) {
      auto [it, inserted] = byName.try_emplace(std::string(d.name), d);
      if (!inserted) {
        throw std::runtime_error(std::format(
            "duplicate device name '{}': {} and {}", d.name,
            describe(it->second), describe(d)));
      }
    }
  }

  // --- (busKey, slaveId) uniqueness across all master-role devices ---
  // Catches e.g. two meters on /dev/ttyUSB0 both at unit_id 1.
  {
    std::map<std::pair<std::string, int>, DeviceRef> byBusSlave;
    for (const auto &d : masters) {
      auto key = std::make_pair(busKey(d), d.slaveId);
      auto [it, inserted] = byBusSlave.try_emplace(key, d);
      if (!inserted) {
        throw std::runtime_error(std::format(
            "{} and {} share bus '{}' and slave id {}",
            describe(it->second), describe(d), key.first, key.second));
      }
    }
  }

  // --- RTU line-parameter consistency on shared bus ---
  // First device on each RTU device path is the reference; subsequent devices
  // on the same path must match. Validates all combinations
  // (inverter+meter, inverter+inverter, meter+meter).
  {
    std::map<std::string, DeviceRef> rtuRef;
    for (const auto &d : masters) {
      if (!d.rtu)
        continue;
      auto [it, inserted] = rtuRef.try_emplace(d.rtu->device, d);
      if (inserted)
        continue;
      const auto &r = *it->second.rtu;
      const auto &o = *d.rtu;
      if (r.baud != o.baud || r.dataBits != o.dataBits ||
          r.stopBits != o.stopBits || r.parity != o.parity) {
        throw std::runtime_error(std::format(
            "{} and {} share RTU device '{}' but have conflicting line "
            "parameters: {} baud/{}-{}-{} vs {} baud/{}-{}-{}",
            describe(it->second), describe(d), r.device, r.baud, r.dataBits,
            parityToChar(r.parity), r.stopBits, o.baud, o.dataBits,
            parityToChar(o.parity), o.stopBits));
      }
    }
  }

  // --- Meter slave validation: TCP listen endpoint uniqueness ---
  // Two slaves cannot bind the same (listen, port). Listing each slave's
  // owning meter in the error makes the conflict locatable.
  {
    struct SlaveRef {
      std::size_t meterIndex;
      std::string_view meterName;
    };
    std::map<std::pair<std::string, int>, SlaveRef> byListen;
    for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
      const auto &m = cfg.meters[i];
      if (!m.slave || !m.slave->tcp)
        continue;
      auto key = std::make_pair(m.slave->tcp->listen, m.slave->tcp->port);
      SlaveRef ref{i, m.name};
      auto [it, inserted] = byListen.try_emplace(key, ref);
      if (!inserted) {
        throw std::runtime_error(std::format(
            "meters[{}] ('{}').slave and meters[{}] ('{}').slave both listen "
            "on {}:{}",
            it->second.meterIndex, it->second.meterName, ref.meterIndex,
            ref.meterName, key.first, key.second));
      }
    }
  }

  // --- RTU device exclusivity across master and slave roles ---
  // No /dev/tty* may be used both as a master (anywhere) and as a slave
  // (any meter), and no two slaves may share an RTU device. Two slaves on
  // one wire is a Modbus impossibility (single-master bus); a master and a
  // slave on the same wire would deadlock the bridge against itself.
  {
    std::map<std::string, std::string> masterUsage; // device -> owner desc
    for (const auto &d : masters) {
      if (d.rtu)
        masterUsage.emplace(d.rtu->device, describe(d));
    }
    std::map<std::string, std::string> slaveUsage;
    for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
      const auto &m = cfg.meters[i];
      if (!m.slave || !m.slave->rtu)
        continue;
      const auto owner = std::format("meters[{}] ('{}').slave", i, m.name);

      if (auto it = masterUsage.find(m.slave->rtu->device);
          it != masterUsage.end()) {
        throw std::runtime_error(std::format(
            "RTU device '{}' is used by master {} and slave {}",
            m.slave->rtu->device, it->second, owner));
      }
      auto [it, inserted] = slaveUsage.try_emplace(m.slave->rtu->device, owner);
      if (!inserted) {
        throw std::runtime_error(std::format(
            "RTU device '{}' is used by two slaves: {} and {}",
            m.slave->rtu->device, it->second, owner));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

AppConfig loadConfig(const std::string &path) {
  YAML::Node root = YAML::LoadFile(path);
  AppConfig cfg;

  cfg.inverters = parseInverters(root["inverters"]);
  cfg.meters = parseMeters(root["meters"]);

  if (cfg.inverters.empty() && cfg.meters.empty())
    throw std::runtime_error(
        "no devices configured (inverters and meters are both empty), "
        "nothing to do");

  cfg.mqtt = parseMqtt(root["mqtt"]);
  cfg.logger = parseLogger(root["logger"]);

  validateConfig(cfg);

  return cfg;
}