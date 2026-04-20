#include "config_yaml.h"
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

// Shared logic for any Modbus master section (inverter or meter.master)
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

static std::optional<InverterConfig> parseInverter(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  try {
    return parseModbusMaster<InverterConfig>(node);
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("inverter") + e.what());
  }
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
    throw std::invalid_argument("meter.slave.unit_id must be in range 1-247");
  if (cfg.requestTimeout <= 0)
    throw std::invalid_argument("meter.slave.request_timeout must be positive");
  if (cfg.idleTimeout < cfg.requestTimeout)
    throw std::invalid_argument(
        "meter.slave.idle_timeout must be >= request_timeout");

  return cfg;
}

static std::optional<MeterConfig> parseMeter(const YAML::Node &node) {
  if (!node)
    return std::nullopt;

  MeterConfig cfg;
  try {
    cfg.master = parseModbusMaster<MeterMasterConfig>(node["master"]);
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("meter.master") + e.what());
  }
  try {
    cfg.slave = parseMeterSlave(node["slave"]);
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("meter.slave") + e.what());
  }

  return cfg;
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

static void validateConfig(const AppConfig &cfg) {
  if (cfg.meter && cfg.meter->slave) {
    const MeterMasterConfig &master = cfg.meter->master;
    const MeterSlaveConfig &slave = *cfg.meter->slave;

    // RTU device conflict: master and slave cannot share the same serial device
    if (master.rtu && slave.rtu && master.rtu->device == slave.rtu->device) {
      throw std::runtime_error(
          "meter.master and meter.slave cannot share the same RTU device");
    }
  }

  if (cfg.meter && cfg.inverter) {
    const MeterMasterConfig &meter = cfg.meter->master;
    const InverterConfig &inverter = *cfg.inverter;

    // RTU device conflict: inverter and meter master cannot share the same
    // device until a shared bus has been implemented
    if (inverter.rtu && meter.rtu &&
        inverter.rtu->device == meter.rtu->device) {
      throw std::runtime_error(
          "meter.master and inverter cannot share the same RTU device");
    }
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

AppConfig loadConfig(const std::string &path) {
  YAML::Node root = YAML::LoadFile(path);
  AppConfig cfg;

  cfg.inverter = parseInverter(root["inverter"]);
  cfg.meter = parseMeter(root["meter"]);

  if (!cfg.inverter && !cfg.meter)
    throw std::runtime_error(
        "neither inverter nor meter configured, nothing to do");

  cfg.mqtt = parseMqtt(root["mqtt"]);
  cfg.logger = parseLogger(root["logger"]);

  validateConfig(cfg);

  return cfg;
}