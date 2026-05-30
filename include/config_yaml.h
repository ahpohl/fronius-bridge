#ifndef CONFIG_YAML_HPP
#define CONFIG_YAML_HPP

#include <fronius/modbus_config.h>
#include <map>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <termios.h>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Serial types
// ---------------------------------------------------------------------------

enum class Parity { None, Even, Odd };

// ---------------------------------------------------------------------------
// Conversion helpers (defined in config_yaml.cpp)
// ---------------------------------------------------------------------------

char parityToChar(Parity parity);
speed_t baudToSpeed(int baud);
tcflag_t dataBitsToFlag(int dataBits);
void applyParity(termios &tty, Parity parity);

// ---------------------------------------------------------------------------
// Modbus transport configs
// ---------------------------------------------------------------------------

struct ModbusTcpClientConfig {
  std::string host;
  int port{502};
};

struct ModbusTcpServerConfig {
  std::string listen{"0.0.0.0"};
  int port{502};
};

struct ModbusRtuConfig {
  std::string device;
  int baud{9600};
  int dataBits{8};
  int stopBits{1};
  Parity parity{Parity::None};
};

// ---------------------------------------------------------------------------
// Shared sub-configs
// ---------------------------------------------------------------------------

struct ReconnectDelayConfig {
  int min{5};
  int max{320};
  bool exponential{true};
};

struct ResponseTimeoutConfig {
  int sec{5};
  int usec{0};
};

// ---------------------------------------------------------------------------
// Meter slave config
//
// Optional per-meter SunSpec endpoint that mirrors the meter's live values
// onto a Modbus slave (typically TCP). The parent meter is the data source;
// this struct only describes how to serve it.
// ---------------------------------------------------------------------------

struct MeterSlaveConfig {
  std::optional<ModbusTcpServerConfig> tcp;
  std::optional<ModbusRtuConfig> rtu;
  int slaveId{1};
  int requestTimeout{5};
  int idleTimeout{60};
  bool useFloatModel{false};
};

// ---------------------------------------------------------------------------
// Inverter config (one entry per physical inverter)
// ---------------------------------------------------------------------------

struct InverterConfig {
  std::string name;
  std::optional<ModbusTcpClientConfig> tcp;
  std::optional<ModbusRtuConfig> rtu;
  int slaveId{1};
  ResponseTimeoutConfig responseTimeout;
  int updateInterval{4};
  ReconnectDelayConfig reconnectDelay;
};

// ---------------------------------------------------------------------------
// Meter config (one entry per physical meter)
//
// A meter entry has a kind-agnostic envelope (`MeterConfig`) carrying the
// fields every meter shares — the device `name` and the optional SunSpec
// `slave` block — plus a `body` variant holding the kind-specific transport
// and protocol configuration.
//
// Two kinds are supported, selected by the YAML `type:` field (default
// "fronius"):
//
//   - FroniusMeterConfig ("fronius"): a SunSpec meter reached over Modbus
//     (TCP or RTU). Participates in the shared-bus registry and is polled on
//     a fixed update interval.
//
//   - EasyMeterConfig ("ebz"): an EBZ Easymeter read passively over a
//     dedicated serial line (SML/OBIS telegrams). It does NOT use Modbus,
//     does NOT join the shared bus, and is event-driven rather than polled.
//     Populated in a later patch; empty for now so the variant type is
//     complete.
// ---------------------------------------------------------------------------

// Fronius SunSpec meter reached over Modbus (TCP or RTU). These are exactly
// the fields the former flat MeterConfig carried, minus the envelope fields
// (`name`, `slave`) which now live on the wrapping MeterConfig.
struct FroniusMeterConfig {
  std::optional<ModbusTcpClientConfig> tcp;
  std::optional<ModbusRtuConfig> rtu;
  int slaveId{1};
  ResponseTimeoutConfig responseTimeout;
  int updateInterval{4};
  ReconnectDelayConfig reconnectDelay;
};

// Grid assumptions for meters that report only active power (e.g. the EBZ
// Easymeter). Reactive/apparent power and energy, and per-phase currents, are
// derived from these assumed values since the meter does not measure them.
struct GridConfig {
  double powerFactor{0.95};
  double frequency{50.0};
  bool isLeading{false};
};

// EBZ Easymeter read passively over a dedicated serial line. Unlike a Fronius
// meter it does not use Modbus: it owns its serial port exclusively (no shared
// bus), is read by waiting on inbound SML/OBIS telegrams (no poll interval,
// no slave id), and derives unmeasured quantities from `grid`.
struct EasyMeterConfig {
  ModbusRtuConfig rtu;
  GridConfig grid;
};

// Kind-agnostic envelope. The optional `slave` block, if present, makes this
// meter's live values available on a SunSpec Modbus endpoint (typically used
// to feed a Fronius inverter for export limiting); it is independent of the
// meter kind in `body`.
struct MeterConfig {
  std::string name;
  std::optional<MeterSlaveConfig> slave;
  std::variant<FroniusMeterConfig, EasyMeterConfig> body;
};

// ---------------------------------------------------------------------------
// MQTT config
// ---------------------------------------------------------------------------

struct MqttConfig {
  std::string broker{"localhost"};
  int port{1883};
  std::string topic;
  std::optional<std::string> user;
  std::optional<std::string> password;
  size_t queueSize{100};
  ReconnectDelayConfig reconnectDelay;
};

// ---------------------------------------------------------------------------
// Logger config
// ---------------------------------------------------------------------------

struct LoggerConfig {
  spdlog::level::level_enum globalLevel{spdlog::level::info};
  std::map<std::string, spdlog::level::level_enum> moduleLevels;
};

// ---------------------------------------------------------------------------
// Derived bus registry
// ---------------------------------------------------------------------------

// One device's place on a shared bus, for the startup summary / diagnostics.
struct BusMember {
  std::string name;
  int slaveId;
};

// A derived bus: the deduplicated libfronius config for one physical bus
// (RS-485 device or TCP endpoint) plus the devices that share it. Synthesised
// by loadConfig() from the inverter and meter sections — there is no [buses]
// YAML section — with reconnect-delay aggregated across the sharing devices.
// Holds config only; it opens no hardware (FroniusBus is built from it later).
struct BusInfo {
  ModbusBusConfig config;
  std::vector<BusMember> members;
};

// Bus key (stable string identity) a device's transport maps to: RTU device
// path, or TCP "host:port". Two devices with the same key share one bus. The
// meter overload returns nullopt for non-bus kinds (the EBZ Easymeter, which
// owns a dedicated serial line and never joins the shared bus). Used by main
// to look a device up in the derived bus registry.
std::optional<std::string> busKeyOf(const MeterConfig &m);
std::string busKeyOf(const InverterConfig &i);

// Human-readable transport descriptor for the startup summary, e.g.
// "RTU 9600 8N1" or "TCP". Reconnect policy is deliberately omitted.
std::string busTransportLabel(const ModbusBusConfig &cfg);

// ---------------------------------------------------------------------------
// Root config
// ---------------------------------------------------------------------------

struct AppConfig {
  std::vector<InverterConfig> inverters;
  std::vector<MeterConfig> meters;
  MqttConfig mqtt;
  LoggerConfig logger;

  // Derived, not parsed: the deduplicated bus registry synthesised from
  // `inverters` and `meters` by loadConfig() (there is no [buses] YAML
  // section). Keyed by bus key (see busKeyOf). main builds one FroniusBus
  // per entry.
  std::map<std::string, BusInfo> buses;
};

AppConfig loadConfig(const std::string &path);

inline const char *opt_c_str(const std::optional<std::string> &s) {
  return s ? s->c_str() : nullptr;
}

// Convenience accessors for the meter `body` variant. Returns nullptr when
// the meter is not of the requested kind, so callers can branch without
// repeating std::get_if at every site.
inline const FroniusMeterConfig *asFronius(const MeterConfig &m) {
  return std::get_if<FroniusMeterConfig>(&m.body);
}
inline const EasyMeterConfig *asEasyMeter(const MeterConfig &m) {
  return std::get_if<EasyMeterConfig>(&m.body);
}

#endif