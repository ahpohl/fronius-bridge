#include "config_yaml.h"
#include <algorithm>
#include <array>
#include <format>
#include <fronius/modbus_config.h>
#include <map>
#include <optional>
#include <regex>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <yaml-cpp/yaml.h>

// ---------------------------------------------------------------------------
// Serial conversion helpers
// ---------------------------------------------------------------------------

namespace {

// Canonical list of serial baud rates the bridge supports, paired with the
// termios speed_t each maps to. Single source of truth for both baudToSpeed()
// (termios setup in the EasyMeter reader) and parseRtu() config
// validation, so an unsupported rate is rejected at startup for every RTU
// device rather than being passed straight to modbus_new_rtu() on the shared
// libfronius bus, where libmodbus silently fails to apply it.
struct BaudEntry {
  int baud;
  speed_t speed;
};

constexpr std::array<BaudEntry, 10> supportedBauds{{
    {1200, B1200},
    {2400, B2400},
    {4800, B4800},
    {9600, B9600},
    {19200, B19200},
    {38400, B38400},
    {57600, B57600},
    {115200, B115200},
    {230400, B230400},
    {460800, B460800},
}};

std::optional<speed_t> lookupBaud(int baud) {
  for (const auto &e : supportedBauds)
    if (e.baud == baud)
      return e.speed;
  return std::nullopt;
}

std::string supportedBaudList() {
  std::string list;
  for (const auto &e : supportedBauds) {
    if (!list.empty())
      list += ", ";
    list += std::to_string(e.baud);
  }
  return list;
}

// Parse a positive count setting (e.g. a queue size) defaulting to `def`.
// Parsed through a signed type so a negative value is rejected with a clear
// error rather than wrapping to an enormous size_t: yaml-cpp's unsigned
// conversion does not reject negatives, so reading straight into size_t would
// let `queue_size: -1` slip past a `== 0` check. `field` names the setting
// for the diagnostic.
std::size_t parsePositiveSize(const YAML::Node &node, const char *field,
                              long long def) {
  const long long value = node.as<long long>(def);
  if (value <= 0)
    throw std::invalid_argument(
        std::format("{} must be greater than zero", field));
  return static_cast<std::size_t>(value);
}

} // namespace

// Map a baud rate to its termios speed_t. The rate must already have been
// accepted by parseRtu(), which validates every RTU device against
// supportedBauds at config load, so a miss here is a programming error
// rather than bad user input.
speed_t baudToSpeed(int baud) {
  if (auto speed = lookupBaud(baud))
    return *speed;
  throw std::logic_error("baudToSpeed called with unvalidated baud rate " +
                         std::to_string(baud));
}

// Map a serial data-bit count to its termios character-size flag. The value
// must already have been accepted by parseRtu() (every RTU device is validated
// at config load), so a miss here is a programming error rather than bad user
// input.
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
    throw std::logic_error("dataBitsToFlag called with unvalidated data bits " +
                           std::to_string(dataBits));
  }
}

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

// Map a parity name to its enum, or std::nullopt for an unknown value.
// Mirrors lookupBaud(): the conversion does not throw, the caller (parseRtu)
// validates, so parity is checked in the same place as the other RTU
// parameters.
static std::optional<Parity> parseParity(const std::string &val) {
  if (val == "none")
    return Parity::None;
  if (val == "even")
    return Parity::Even;
  if (val == "odd")
    return Parity::Odd;
  return std::nullopt;
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
    throw std::invalid_argument(".tcp.port must be in range [1-65535]");

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
    throw std::invalid_argument(".tcp.port must be in range [1-65535]");

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
  auto parity = parseParity(node["parity"].as<std::string>("none"));

  if (!lookupBaud(rtu.baud))
    throw std::invalid_argument(
        ".rtu.baud " + std::to_string(rtu.baud) +
        " is not a supported rate (supported: " + supportedBaudList() + ")");
  if (rtu.dataBits < 5 || rtu.dataBits > 8)
    throw std::invalid_argument(".rtu.data_bits must be [5,6,7,8]");
  if (rtu.stopBits != 1 && rtu.stopBits != 2)
    throw std::invalid_argument(".rtu.stop_bits must be [1,2]");
  if (!parity)
    throw std::invalid_argument(".rtu.parity must be [none, even, odd]");

  rtu.parity = *parity;
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

  // libmodbus modbus_set_response_timeout() returns EINVAL when usec is
  // outside [0, 999999] or when sec and usec are both zero; reject those at
  // load so the config fails here rather than at connect.
  if (cfg.sec < 0)
    throw std::invalid_argument(".response_timeout.sec must be non-negative");
  if (cfg.usec < 0 || cfg.usec >= 1000000)
    throw std::invalid_argument(
        ".response_timeout.usec must be in range [0-999999]");
  if (cfg.sec == 0 && cfg.usec == 0)
    throw std::invalid_argument(
        ".response_timeout sec and usec must not both be zero");

  return cfg;
}

// ---------------------------------------------------------------------------
// Name parsing and validation
//
// Device names are used as MQTT topic segments (between the configured base
// topic and the per-device suffixes such as `/values`, `/availability`).
// They are no longer used as logger names — loggers are now fixed
// class-based modules (`meter`, `meter.master`, `meter.slave`, `inverter`),
// independent of how many devices are configured.
//
// Restrict to a safe character set so names cannot break MQTT topics
// (no '+', '#', '/') or surprise downstream consumers.
//
// `meter` and `inverter` are reserved because they would produce a topic
// like `<base>/meter/meter/values` or `<base>/inverter/inverter/values`
// which is parseable but visually confusing for anyone reading the topic
// stream.
//
// Throws messages prefixed with `:` so the calling parser prepends its own
// section label (`inverters[0]`, `meters[1]`, …) via the surrounding
// try/catch.
// ---------------------------------------------------------------------------

// A bare `name:` (or `name: ~` / `name: null`) parses as a YAML null, which
// yaml-cpp converts to the string "null" rather than to an empty string or a
// failed conversion; treat a missing or null node as empty so it is reported
// as the unset field it is rather than a device literally named "null".
static std::string parseName(const YAML::Node &node) {
  const auto n = node["name"];
  const std::string name = (n && !n.IsNull()) ? n.as<std::string>("") : "";

  if (name.empty())
    throw std::invalid_argument(": .name must not be empty");
  if (name.size() > 32)
    throw std::invalid_argument(": .name must be 32 characters or fewer");

  static const std::regex pattern("[A-Za-z0-9_-]+");
  if (!std::regex_match(name, pattern))
    throw std::invalid_argument(
        ": .name must contain only [A-Za-z0-9_-] characters");

  if (name == "meter" || name == "inverter")
    throw std::invalid_argument(
        std::string(": .name '") + name +
        "' is reserved (would produce a visually ambiguous topic of "
        "the form <base>/" +
        name + "/" + name + "/...)");

  return name;
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
    throw std::invalid_argument(".unit_id must be in range [1-247]");
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
    throw std::runtime_error("inverters must be a sequence of entries, each "
                             "starting with '- ' and containing a 'name'");

  result.reserve(node.size());
  for (std::size_t i = 0; i < node.size(); ++i) {
    const auto prefix = std::format("inverters[{}]", i);
    try {
      auto cfg = parseModbusMaster<InverterConfig>(node[i]);
      cfg.name = parseName(node[i]);
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
    throw std::invalid_argument(".unit_id must be in range [1-247]");
  if (cfg.requestTimeout <= 0)
    throw std::invalid_argument(".request_timeout must be positive");
  if (cfg.idleTimeout < cfg.requestTimeout)
    throw std::invalid_argument(".idle_timeout must be >= request_timeout");

  return cfg;
}

static GridConfig parseGrid(const YAML::Node &node) {
  GridConfig cfg;
  if (!node)
    return cfg;

  cfg.powerFactor = node["power_factor"].as<double>(0.95);
  cfg.frequency = node["frequency"].as<double>(50.0);
  cfg.isLeading = node["leading"].as<bool>(false);

  if (cfg.powerFactor <= 0.0 || cfg.powerFactor > 1.0)
    throw std::invalid_argument(
        ".grid.power_factor must be in range [0.0, 1.0]");
  if (cfg.frequency <= 0.0)
    throw std::invalid_argument(".grid.frequency must be positive");

  return cfg;
}

// Parse the body of a `type: ebz` meter. The EasyMeter is read passively
// over a dedicated serial line and is not a Modbus device, so it accepts only
// `rtu` (required) and `grid` (optional). Modbus-only keys are rejected
// rather than silently ignored, so a config that carries them under an EBZ
// meter is caught as the mistake it is instead of behaving unexpectedly.
static EasyMeterConfig parseEasyMeter(const YAML::Node &node) {
  EasyMeterConfig cfg;

  // Reject Modbus-only / Fronius-only keys that have no meaning for the EBZ.
  static const std::pair<const char *, const char *> rejected[] = {
      {"tcp", "the EasyMeter has no network transport (use rtu)"},
      {"unit_id", "the EasyMeter is not a Modbus device"},
      {"update_interval",
       "the EasyMeter publishes on telegram arrival, not on an interval"},
      {"response_timeout", "the EasyMeter is not a Modbus device"},
      {"reconnect_delay", "the EasyMeter manages its own serial reconnect"},
  };
  for (const auto &[key, why] : rejected)
    if (node[key])
      throw std::runtime_error(std::string(".") + key +
                               " is not valid for type 'ebz': " + why);

  auto rtu = parseRtu(node["rtu"]);
  if (!rtu)
    throw std::runtime_error(".rtu is required for type 'ebz'");
  cfg.rtu = std::move(*rtu);

  cfg.grid = parseGrid(node["grid"]);

  return cfg;
}

// "feed-in" or "consumption"; nullopt for any other string (the caller turns
// that into a fatal error). Absence of the key is handled by the caller, not
// here, so that a present-but-misspelled value is rejected rather than ignored.
static std::optional<MeterLocation> parseMeterLocation(const std::string &val) {
  if (val == "feed-in")
    return MeterLocation::FeedIn;
  if (val == "consumption")
    return MeterLocation::Consumption;
  return std::nullopt;
}

static std::vector<MeterConfig> parseMeters(const YAML::Node &node) {
  std::vector<MeterConfig> result;
  if (!node)
    return result;

  if (!node.IsSequence())
    throw std::runtime_error("meters must be a sequence of entries, each "
                             "starting with '- ' and containing a 'name'");

  result.reserve(node.size());
  for (std::size_t i = 0; i < node.size(); ++i) {
    const auto prefix = std::format("meters[{}]", i);
    try {
      // Envelope fields shared by every meter kind.
      MeterConfig cfg;
      cfg.name = parseName(node[i]);

      try {
        cfg.slave = parseMeterSlave(node[i]["slave"]);
      } catch (const std::exception &e) {
        throw std::runtime_error(std::string(".slave") + e.what());
      }

      // Role in the site energy model (both optional). `location` selects how
      // this meter's energy is interpreted; `primary` marks the main reference
      // meter and therefore requires a location.
      if (const auto loc = node[i]["location"]) {
        const auto s = loc.as<std::string>();
        const auto parsed = parseMeterLocation(s);
        if (!parsed)
          throw std::runtime_error(".location: unknown value '" + s +
                                   "' (expected 'feed-in' or 'consumption')");
        cfg.location = *parsed;
      }
      cfg.primary = node[i]["primary"].as<bool>(false);
      if (cfg.primary && !cfg.location)
        throw std::runtime_error(
            ".primary requires .location ('feed-in' or 'consumption')");

      // Kind-specific body, selected by the `type` field. "fronius" (the
      // default) is a SunSpec meter over Modbus; "ebz" is an EasyMeter
      // read over a dedicated serial line.
      const auto type = node[i]["type"].as<std::string>("fronius");
      if (type == "fronius") {
        // Catch the easy mistake of writing an EBZ meter without `type: ebz`.
        // The yaml parser would otherwise silently ignore the EBZ-only keys
        // and treat the entry as a Fronius meter, which then fails at
        // connect-time with a Modbus timeout rather than a clear config
        // error.
        if (node[i]["grid"])
          throw std::runtime_error(
              ".grid is only valid for type: ebz (the EasyMeter); "
              "add 'type: ebz' or remove the 'grid' block");
        cfg.body = parseModbusMaster<FroniusMeterConfig>(node[i]);
      } else if (type == "ebz") {
        cfg.body = parseEasyMeter(node[i]);
      } else {
        throw std::runtime_error(".type: unknown meter type '" + type +
                                 "' (expected 'fronius' or 'ebz')");
      }

      result.push_back(std::move(cfg));
    } catch (const std::exception &e) {
      throw std::runtime_error(prefix + e.what());
    }
  }

  // At most one primary meter: it is the single site-level reference, and its
  // location selects how the site figures are computed. Zero is fine (the site
  // energy feature is simply inactive); two or more is always a mistake.
  if (std::count_if(result.begin(), result.end(),
                    [](const MeterConfig &m) { return m.primary; }) > 1)
    throw std::runtime_error(
        "meters: at most one meter may be marked 'primary: true'");

  return result;
}

static MqttConfig parseMqtt(const YAML::Node &node) {
  if (!node)
    throw std::runtime_error("Missing mqtt section in config");

  MqttConfig cfg;
  cfg.broker = node["broker"].as<std::string>("localhost");
  cfg.port = node["port"].as<int>(1883);
  cfg.topic = node["topic"].as<std::string>("fronius-bridge");
  cfg.queueSize = parsePositiveSize(node["queue_size"], "mqtt.queue_size", 100);

  if (node["user"])
    cfg.user = node["user"].as<std::string>();
  if (node["password"])
    cfg.password = node["password"].as<std::string>();

  cfg.reconnectDelay = parseReconnectDelay(node["reconnect_delay"]);

  if (cfg.port <= 0 || cfg.port > 65535)
    throw std::invalid_argument("mqtt.port must be in range [1-65535]");

  return cfg;
}

// The postgres section is optional: a missing section returns nullopt and the
// bridge runs MQTT-only. When present, `dsn` is mandatory (there is no usable
// default for a connection string); queue_size and reconnect_delay mirror the
// mqtt semantics. autoMigrate is intentionally not read here — it is a runtime
// flag set from the CLI, not config.
static std::optional<PostgresConfig> parsePostgres(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  PostgresConfig cfg;

  const auto dsn = node["dsn"];
  cfg.dsn = (dsn && !dsn.IsNull()) ? dsn.as<std::string>("") : "";
  if (cfg.dsn.empty())
    throw std::invalid_argument(
        "postgres.dsn is required when the postgres section is present");

  cfg.queueSize =
      parsePositiveSize(node["queue_size"], "postgres.queue_size", 10000);
  cfg.reconnectDelay = parseReconnectDelay(node["reconnect_delay"]);

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

// Parse the optional `site:` section: the installation's latitude/longitude.
// Both keys are optional and validated to real-world ranges when present; an
// absent section leaves both unset (no daylight figure is derived). Only
// latitude feeds the daylight calculation; longitude is stored for future use.
static SiteConfig parseSite(const YAML::Node &node) {
  SiteConfig cfg;
  if (!node)
    return cfg;

  if (node["latitude"]) {
    const double v = node["latitude"].as<double>();
    if (v < -90.0 || v > 90.0)
      throw std::invalid_argument("site.latitude must be in range [-90, 90]");
    cfg.latitude = v;
  }
  if (node["longitude"]) {
    const double v = node["longitude"].as<double>();
    if (v < -180.0 || v > 180.0)
      throw std::invalid_argument(
          "site.longitude must be in range [-180, 180]");
    cfg.longitude = v;
  }
  if (node["horizon"]) {
    const double v = node["horizon"].as<double>();
    if (v < -18.0 || v > 20.0)
      throw std::invalid_argument("site.horizon must be in range [-18, 20]");
    cfg.horizon = v;
  }

  return cfg;
}

// ---------------------------------------------------------------------------
// Cross section validation
// ---------------------------------------------------------------------------

namespace {

// Stable string identity for a bus (RTU device path, or TCP host:port).
std::string busKeyTcp(const ModbusTcpClientConfig &tcp) {
  return tcp.host + ":" + std::to_string(tcp.port);
}

std::string busKeyRtu(const ModbusRtuConfig &rtu) { return rtu.device; }

// A uniform view of a master-role device (inverter or meter) for the
// purposes of cross-section validation. Pointers reference fields inside
// the AppConfig that the surrounding validateConfig() call owns, so
// DeviceRef must not outlive that call.
struct DeviceRef {
  std::string_view kind; // "inverters" or "meters"
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

// Translate a single device's config into the libfronius bus config it would
// open (transport + reconnect policy + wire-trace flag). Pure config->config
// transformation; opens no hardware. FroniusMeterConfig and InverterConfig
// share the same .tcp/.rtu/.reconnectDelay shape, so one template serves both.
template <typename Cfg> ModbusBusConfig makeBusConfig(const Cfg &cfg) {
  ModbusBusConfig busCfg;

  if (cfg.tcp) {
    busCfg.transport = ModbusTcpTransport{
        .host = cfg.tcp->host,
        .port = cfg.tcp->port,
    };
  } else if (cfg.rtu) {
    busCfg.transport = ModbusRtuTransport{
        .device = cfg.rtu->device,
        .baud = cfg.rtu->baud,
        .dataBits = cfg.rtu->dataBits,
        .stopBits = cfg.rtu->stopBits,
        .parity = parityToChar(cfg.rtu->parity),
    };
  } else {
    // Defensive: parsing already requires exactly one transport per device.
    throw std::runtime_error("device has no transport configured (need tcp or "
                             "rtu)");
  }

  // busCfg.debug (the libmodbus wire trace) is intentionally left at its
  // default of false here. Whether to enable it depends on the 'bus' logger
  // being at trace level, but deriveBuses() runs inside loadConfig() — before
  // setupLogging() has registered any logger — so spdlog::get("bus") would
  // always return nullptr at this point. The decision is therefore deferred to
  // main(), which sets the flag per bus once the module loggers exist (see the
  // bus-construction loop after setupLogging).

  busCfg.reconnectDelay = cfg.reconnectDelay.min;
  busCfg.reconnectDelayMax = cfg.reconnectDelay.max;
  busCfg.exponential = cfg.reconnectDelay.exponential;

  return busCfg;
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
  dst.reconnectDelayMax =
      std::min(dst.reconnectDelayMax, src.reconnectDelayMax);
  dst.exponential = dst.exponential || src.exponential;
}

// What a meter contributes to the shared-bus registry: the bus it joins, the
// config it opens, and the slave id it answers on. Dispatch is on the
// meter-kind variant so the registry build stays kind-agnostic: Fronius
// (Modbus) meters return a populated entry; the EBZ Easymeter owns a dedicated
// serial line and never joins the shared bus, so it returns nullopt. The visit
// is exhaustive, forcing an explicit decision for any future meter kind.
struct MeterBusEntry {
  std::string key;
  ModbusBusConfig config;
  int slaveId;
};

std::optional<MeterBusEntry> meterBusEntry(const MeterConfig &m) {
  return std::visit(
      [](const auto &body) -> std::optional<MeterBusEntry> {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, FroniusMeterConfig>) {
          std::string key =
              body.rtu ? busKeyRtu(*body.rtu) : busKeyTcp(*body.tcp);
          return MeterBusEntry{std::move(key), makeBusConfig(body),
                               body.slaveId};
        } else if constexpr (std::is_same_v<T, EasyMeterConfig>) {
          return std::nullopt;
        } else {
          static_assert(sizeof(T) == 0, "unhandled meter kind");
        }
      },
      m.body);
}

} // namespace

std::optional<std::string> busKeyOf(const MeterConfig &m) {
  auto entry = meterBusEntry(m);
  return entry ? std::optional<std::string>(std::move(entry->key))
               : std::nullopt;
}

std::string busKeyOf(const InverterConfig &i) {
  return i.rtu ? busKeyRtu(*i.rtu) : busKeyTcp(*i.tcp);
}

std::string busSummaryLine(const std::string &key, const BusInfo &info) {
  std::string members;
  const auto &m = info.members;
  for (std::size_t i = 0; i < m.size(); ++i) {
    if (i > 0)
      members += (i + 1 == m.size()) ? ", and " : ", ";
    members += std::format("'{}' (id {})", m[i].name, m[i].slaveId);
  }
  if (info.config.isRtu()) {
    const auto &r = info.config.rtu();
    return std::format(
        "{} with RTU transport ({}{}{}, {} baud) assigned to '{}'", members,
        r.dataBits, r.parity, r.stopBits, r.baud, key);
  }
  // TCP is point-to-point, not a shared medium; the key is the host:port the
  // device(s) connect to.
  return std::format("{} with TCP transport connecting to '{}'", members, key);
}

// Synthesise the deduplicated bus registry from the inverter and meter
// sections. Each unique transport (RTU device path or TCP host:port) becomes
// one BusInfo; devices that share it are aggregated — reconnect-delay merged
// to a single policy (see mergeReconnectDelay) and recorded as members. The
// EBZ Easymeter contributes nothing (meterBusEntry returns nullopt). No
// hardware is opened here. Call after validateConfig(), which guarantees that
// devices sharing a bus already agree on line parameters.
static std::map<std::string, BusInfo> deriveBuses(const AppConfig &cfg) {
  std::map<std::string, BusInfo> buses;

  auto fold = [&](const std::string &key, const ModbusBusConfig &busCfg,
                  std::string name, int slaveId) {
    auto [it, inserted] = buses.try_emplace(key, BusInfo{busCfg, {}});
    if (!inserted)
      mergeReconnectDelay(it->second.config, busCfg);
    it->second.members.push_back({std::move(name), slaveId});
  };

  for (const auto &m : cfg.meters)
    if (auto e = meterBusEntry(m))
      fold(e->key, e->config, m.name, e->slaveId);
  for (const auto &i : cfg.inverters)
    fold(busKeyOf(i), makeBusConfig(i), i.name, i.slaveId);

  return buses;
}

// Canonical registry string for a meter location, the inverse of
// parseMeterLocation. Kept beside it so the two directions stay in sync.
static std::string_view meterLocationName(MeterLocation loc) {
  return loc == MeterLocation::FeedIn ? "feed-in" : "consumption";
}

// One registry entry per configured device, in section order: inverters first
// (kind 'inverter', no location), then meters (kind 'meter', carrying their
// location and primary role). The PostgreSQL consumer writes these into
// public.device_registry at startup.
static std::vector<DeviceRegistryEntry>
deriveDeviceRegistry(const AppConfig &cfg) {
  std::vector<DeviceRegistryEntry> registry;
  registry.reserve(cfg.inverters.size() + cfg.meters.size());

  for (const auto &i : cfg.inverters)
    registry.push_back({i.name, "inverter", std::nullopt, false});
  for (const auto &m : cfg.meters) {
    std::optional<std::string> location;
    if (m.location)
      location = std::string{meterLocationName(*m.location)};
    registry.push_back({m.name, "meter", std::move(location), m.primary});
  }

  return registry;
}

static void validateConfig(const AppConfig &cfg) {
  // --- Collect master-role devices in a uniform form ---
  std::vector<DeviceRef> masters;
  masters.reserve(cfg.inverters.size() + cfg.meters.size());

  for (std::size_t i = 0; i < cfg.inverters.size(); ++i) {
    const auto &c = cfg.inverters[i];
    masters.push_back({"inverters", i, c.name, c.tcp ? &*c.tcp : nullptr,
                       c.rtu ? &*c.rtu : nullptr, c.slaveId});
  }
  for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
    const auto &c = cfg.meters[i];
    // Only Fronius (Modbus) meters are master-role devices on the bus. Other
    // kinds (e.g. EBZ) do not join the shared bus and are excluded from the
    // bus-level checks (busKey/slaveId uniqueness, RTU line-parameter
    // consistency); they participate in the name-uniqueness and RTU device
    // exclusivity checks below via their own collection.
    const auto *f = asFronius(c);
    if (!f)
      continue;
    masters.push_back({"meters", i, c.name, f->tcp ? &*f->tcp : nullptr,
                       f->rtu ? &*f->rtu : nullptr, f->slaveId});
  }

  // --- Collect the EBZ meter (non-bus, exclusive-serial reader) ---
  // The EasyMeter is neither a Modbus master nor a slave: it owns its serial
  // line exclusively and is read passively. EBZ meters are grid meters, of
  // which an installation has exactly one, so at most one `type: ebz` meter is
  // allowed (a second is rejected here). The single EBZ is kept out of the
  // bus-level checks above but takes part in name uniqueness and the RTU
  // device exclusivity check (its line must not be shared with anything).
  struct EbzRef {
    std::size_t index;
    std::string_view name;
    const ModbusRtuConfig *rtu;
  };
  std::optional<EbzRef> ebzMeter;
  for (std::size_t i = 0; i < cfg.meters.size(); ++i) {
    const auto *e = asEasyMeter(cfg.meters[i]);
    if (!e)
      continue;
    if (ebzMeter)
      throw std::runtime_error(
          std::format("at most one meter of type ebz is allowed, but found "
                      "meters[{}] ('{}') and meters[{}] ('{}')",
                      ebzMeter->index, ebzMeter->name, i, cfg.meters[i].name));
    ebzMeter = EbzRef{i, cfg.meters[i].name, &e->rtu};
  }

  // --- Name uniqueness across all named devices ---
  // Names are used as MQTT topic segments and as logger suffixes;
  // collisions between inverter, meter (Fronius or EBZ) names would be
  // ambiguous, so the scope is global rather than per-kind.
  {
    std::map<std::string, std::string> byName; // name -> owner description
    for (const auto &d : masters) {
      auto [it, inserted] =
          byName.try_emplace(std::string(d.name), describe(d));
      if (!inserted) {
        throw std::runtime_error(
            std::format("duplicate device name '{}': {} and {}", d.name,
                        it->second, describe(d)));
      }
    }
    if (ebzMeter) {
      const auto owner =
          std::format("meters[{}] ('{}')", ebzMeter->index, ebzMeter->name);
      auto [it, inserted] =
          byName.try_emplace(std::string(ebzMeter->name), owner);
      if (!inserted) {
        throw std::runtime_error(
            std::format("duplicate device name '{}': {} and {}", ebzMeter->name,
                        it->second, owner));
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
            "unit_id must be unique among devices sharing a bus: {} and {} "
            "both use unit_id {} on bus '{}'",
            describe(it->second), describe(d), key.second, key.first));
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
            "two slaves cannot bind to the same address and port combination: "
            "meters[{}].slave ('{}') and meters[{}].slave ('{}') both listen "
            "on {}:{}",
            it->second.meterIndex, it->second.meterName, ref.meterIndex,
            ref.meterName, key.first, key.second));
      }
    }
  }

  // --- RTU device exclusivity across master, slave, and EBZ roles ---
  // Rules per /dev/tty* path:
  //   - Multiple Modbus masters may share a path (that is the shared RTU
  //     bus; line-parameter consistency is checked above).
  //   - A meter slave needs the path to itself: no master, no other slave.
  //   - An EasyMeter needs the path to itself exclusively: no master,
  //     no slave, no other EBZ. It locks the device (flock + TIOCEXCL) and
  //     reads passively, so it cannot coexist with anything.
  // A master+slave on one wire would deadlock the bridge against itself;
  // two slaves, or anything sharing with an EBZ, is simply unworkable.
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
      const auto owner = std::format("meters[{}].slave ('{}')", i, m.name);

      if (auto it = masterUsage.find(m.slave->rtu->device);
          it != masterUsage.end()) {
        throw std::runtime_error(std::format(
            "a Modbus master and a meter slave cannot share a RTU device: "
            "'{}' is used by {} and {}",
            m.slave->rtu->device, it->second, owner));
      }
      auto [it, inserted] = slaveUsage.try_emplace(m.slave->rtu->device, owner);
      if (!inserted) {
        throw std::runtime_error(std::format(
            "two meter slaves cannot share a RTU device: '{}' is used by "
            "{} and {}",
            m.slave->rtu->device, it->second, owner));
      }
    }

    // EasyMeter: the serial line must be exclusive. Reject any overlap with
    // a Modbus master or a meter slave. At most one EasyMeter exists (enforced
    // above), so there is no EasyMeter-vs-EasyMeter case.
    if (ebzMeter) {
      const auto &device = ebzMeter->rtu->device;
      const auto owner =
          std::format("meters[{}] ('{}')", ebzMeter->index, ebzMeter->name);

      if (auto it = masterUsage.find(device); it != masterUsage.end()) {
        throw std::runtime_error(std::format(
            "a Modbus master and an EasyMeter cannot share a RTU device: "
            "'{}' is used by {} and {}",
            device, it->second, owner));
      }
      if (auto it = slaveUsage.find(device); it != slaveUsage.end()) {
        throw std::runtime_error(std::format(
            "a meter slave and an EasyMeter cannot share a RTU device: "
            "'{}' is used by {} and {}",
            device, it->second, owner));
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
  cfg.postgres = parsePostgres(root["postgres"]);
  cfg.logger = parseLogger(root["logger"]);
  cfg.site = parseSite(root["site"]);

  validateConfig(cfg);

  // Derive the deduplicated bus registry from the validated device sections.
  cfg.buses = deriveBuses(cfg);

  // Derive the device roster the PostgreSQL consumer writes to the registry.
  cfg.deviceRegistry = deriveDeviceRegistry(cfg);

  return cfg;
}