#ifndef CONFIG_YAML_HPP
#define CONFIG_YAML_HPP

#include <fronius/fronius.h>
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

// Where a meter sits in the installation, in Fronius terms. It determines what
// the meter's energy means for the site-level calculations (a later feature):
// a feed-in meter measures grid exchange (import and export), a consumption
// meter measures a load. Meters with no location are monitored but left out of
// the site energy balance.
enum class MeterLocation { FeedIn, Consumption };

// Kind-agnostic envelope. The optional `slave` block, if present, makes this
// meter's live values available on a SunSpec Modbus endpoint (typically used
// to feed a Fronius inverter for export limiting); it is independent of the
// meter kind in `body`.
//
// `location` and `primary` describe the meter's role for the site energy
// calculations. `location` is unset for meters that are only monitored;
// `primary` marks the single main reference meter (Fronius "primary meter")
// and requires a location, since the primary's location selects how the site
// figures are computed.
struct MeterConfig {
  std::string name;
  std::optional<MeterSlaveConfig> slave;
  std::optional<MeterLocation> location;
  bool primary{false};
  std::variant<FroniusMeterConfig, EasyMeterConfig> body;
};

// ---------------------------------------------------------------------------
// MQTT TLS config
//
// Optional sub-block of the MQTT section. Present means the broker connection
// is secured with TLS; absent means a plaintext connection. The broker
// certificate is verified against `caFile`/`caPath` when either is given, or
// against the OS trust store otherwise (so a broker using a public CA such as
// Let's Encrypt needs no local certificate). `certFile`/`keyFile` add a client
// certificate for mutual TLS and require a CA source, since libmosquitto's
// mosquitto_tls_set() needs cafile or capath whenever a client certificate is
// supplied. `insecure` disables broker hostname/certificate verification and
// is intended for testing against self-signed certificates only.
// ---------------------------------------------------------------------------

struct MqttTlsConfig {
  std::optional<std::string> caFile;     // PEM CA bundle that signed the broker
  std::optional<std::string> caPath;     // directory of PEM CA certificates
  std::optional<std::string> certFile;   // client certificate (mutual TLS)
  std::optional<std::string> keyFile;    // client private key (mutual TLS)
  std::optional<std::string> tlsVersion; // e.g. "tlsv1.2", "tlsv1.3"
  std::optional<std::string> ciphers;    // OpenSSL cipher list
  bool insecure{false};                  // skip broker verification (testing)
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
  std::optional<MqttTlsConfig> tls;
};

// ---------------------------------------------------------------------------
// PostgreSQL config
//
// Optional consumer, peer to MQTT. Present only when the `postgres:` section
// exists in the YAML; absent (std::nullopt on AppConfig) means the bridge
// runs MQTT-only and never opens a database connection. Each named device
// gets its own schema (named after the device); there is no central device
// registry, so this block carries only connection-level settings.
//
// `dsn` is a standard libpq connection string. `queueSize` bounds the
// in-memory FIFO of pending writes (drop-oldest on overflow, as for MQTT).
// `autoMigrate` is not parsed from YAML: it defaults to true and is cleared
// by the CLI `--no-migrate` flag to run schema verification only.
// ---------------------------------------------------------------------------

struct PostgresConfig {
  std::string dsn;
  std::size_t queueSize{10000};
  ReconnectDelayConfig reconnectDelay;
  bool autoMigrate{true}; // CLI-controlled (--no-migrate), not parsed
};

// ---------------------------------------------------------------------------
// Logger config
// ---------------------------------------------------------------------------

struct LoggerConfig {
  spdlog::level::level_enum globalLevel{spdlog::level::info};
  std::map<std::string, spdlog::level::level_enum> moduleLevels;
};

// ---------------------------------------------------------------------------
// Site config
//
// Installation location, written to the single-row public.site table at
// startup. Latitude drives the daylight length; longitude sets the solar
// clock; horizon is the sun-centre altitude counted as sunrise/sunset
// (-0.833 deg geometric default), a per-site calibration for how far outside
// geometric daylight the inverter reports. The struct is optional in AppConfig
// and absent when there is no `site:` section; a present section must supply
// latitude and longitude. Site-level, not per-device: one bridge instance
// serves one site.
// ---------------------------------------------------------------------------

struct SiteConfig {
  double latitude{0.0};  // degrees north [-90, 90]; required when site present
  double longitude{0.0}; // degrees east [-180, 180]; required when site present
  double horizon{-0.833}; // sun-centre altitude at sunrise/sunset, in degrees
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

// Complete startup summary line for one bus. For a shared RTU bus, e.g.
// "'heatpump' (id 2), and 'primo' (id 1) with RTU transport (8N1, 9600 baud)
// assigned to '/dev/ttyUSB0'"; for a point-to-point TCP endpoint, e.g.
// "'primo' (id 1) with TCP transport connecting to '192.168.6.51:502'". Device
// names are quoted and joined with an Oxford comma. Total over both transports,
// so the caller need not pre-filter.
std::string busSummaryLine(const std::string &key, const BusInfo &info);

// A derived per-device descriptor: one device's identity and site-energy role,
// synthesised by loadConfig() from the inverter and meter sections. The bridge
// writes the set verbatim into public.device_registry at startup so the
// site-level SQL can resolve each device's role by name. `location` is the
// canonical string ('feed-in'/'consumption') or nullopt; the consumer binds it
// straight into SQL, so the enum is mapped to text here rather than there.
struct DeviceRegistryEntry {
  std::string name;                    // device name == its schema name
  std::string kind;                    // "inverter" | "meter"
  std::optional<std::string> location; // "feed-in" | "consumption" | nullopt
  bool primary{false};
};

// ---------------------------------------------------------------------------
// Root config
// ---------------------------------------------------------------------------

struct AppConfig {
  std::vector<InverterConfig> inverters;
  std::vector<MeterConfig> meters;
  MqttConfig mqtt;
  std::optional<PostgresConfig> postgres;
  LoggerConfig logger;
  std::optional<SiteConfig> site;

  // Derived, not parsed: the deduplicated bus registry synthesised from
  // `inverters` and `meters` by loadConfig() (there is no [buses] YAML
  // section). Keyed by bus key (see busKeyOf). main builds one FroniusBus
  // per entry.
  std::map<std::string, BusInfo> buses;

  // Derived, not parsed: one entry per configured device, in section order,
  // handed to the PostgreSQL consumer to populate public.device_registry.
  std::vector<DeviceRegistryEntry> deviceRegistry;
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