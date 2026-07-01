#ifndef POSTGRES_CLIENT_H_
#define POSTGRES_CLIENT_H_

#include "config_yaml.h"
#include "db_error.h"
#include "inverter_types.h"
#include "meter_types.h"
#include "signal_handler.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <spdlog/logger.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// Forward-declared so this header stays free of <libpq-fe.h>: the connection is
// held by unique_ptr and only postgres_client.cpp needs the complete type.
// This keeps libpq out of every TU that merely constructs the consumer
// (notably main.cpp).
namespace pg {
class Conn;
}

// ---------------------------------------------------------------------------
// PostgresClient
//
// Optional time-series consumer for inverter and meter data, peer to
// MqttClient. Receives typed device and value structs via callbacks, enqueues
// them onto a bounded in-memory queue, and writes them out from a single worker
// thread that owns the libpq connection.
//
// Per-device schemas: each configured device gets its own PostgreSQL schema
// named after the device (e.g. "primo", "grid", "heatpump"). There is no
// central device registry and no device_id - identity is the schema. The
// device's configured `name`, threaded through every callback, is both the
// schema name and the key into the in-memory caches. On a device's first
// Device event the worker lazily creates/verifies its schema (via
// SchemaMigrator) and builds the schema-qualified SQL it will reuse for that
// device; value rows then insert straight into that schema.
//
// All fallible setup beyond config validation (connect, extension check,
// schema migration, device upserts) happens on the worker thread so a
// transient DB outage at startup does not block the rest of the bridge.
//
// Lifetime: held as std::unique_ptr<PostgresClient> in main(); the
// destructor wakes and joins the worker (std::jthread).
// ---------------------------------------------------------------------------

class PostgresClient {
public:
  // Throws std::invalid_argument on bad config (empty DSN). Connection
  // failures are not raised here - the worker handles them via the reconnect
  // loop. autoMigrate (from cfg) selects migrate vs verify per schema.
  PostgresClient(const PostgresConfig &cfg,
                 std::vector<DeviceRegistryEntry> registry,
                 std::optional<SiteConfig> site, SignalHandler &signalHandler);

  ~PostgresClient();

  PostgresClient(const PostgresClient &) = delete;
  PostgresClient &operator=(const PostgresClient &) = delete;
  PostgresClient(PostgresClient &&) = delete;
  PostgresClient &operator=(PostgresClient &&) = delete;

  // Producer-side callbacks, wired in main() to InverterMaster / MeterMaster.
  // Non-blocking: they enqueue and return. Safe to call from any thread.
  // `deviceName` is the configured device name; it selects the schema and the
  // cache slot the worker routes to.
  void onInverterDevice(std::string deviceName, InverterTypes::Device dev);
  void onMeterDevice(std::string deviceName, MeterTypes::Device dev);
  void onInverter(std::string deviceName, InverterTypes::Values values);
  void onMeter(std::string deviceName, MeterTypes::Values values);

private:
  // Tagged payload for the worker queue. `deviceName` carries the schema /
  // cache identity so the worker need not inspect the payload to route it.
  struct Event {
    std::string deviceName;
    std::variant<InverterTypes::Device, MeterTypes::Device,
                 InverterTypes::Values, MeterTypes::Values>
        payload;
  };

  // Producer-side: push respecting the overflow policy (drop-oldest), with
  // rate-limited drop logging.
  void enqueue(Event ev);

  // Worker entry point. Owns the connection and the caches.
  void run();

  // Worker helpers.
  std::expected<void, DbError> connectAndPrepare();
  std::expected<void, DbError> processEvent(const Event &ev);

  // Upsert the configured device roster into public.device_registry and drop
  // rows for devices no longer configured. Runs once per process, after the
  // public schema is brought up to date.
  std::expected<void, DbError> syncRegistry();

  std::expected<void, DbError>
  upsertInverterDevice(const std::string &name,
                       const InverterTypes::Device &dev);
  std::expected<void, DbError> upsertMeterDevice(const std::string &name,
                                                 const MeterTypes::Device &dev);
  std::expected<void, DbError>
  insertInverterValues(const std::string &name, const InverterTypes::Values &v);
  std::expected<void, DbError> insertMeterValues(const std::string &name,
                                                 const MeterTypes::Values &v);

  // Sleep with backoff, observing handler_.isRunning() so shutdown is
  // responsive.
  void sleepBackoff(std::chrono::seconds duration);

  // Open the libpq SQL trace file (lazy, once per process) and attach the
  // current connection. Guarded by the caller on trace log level.
  void attachSqlTrace();

  // ------ config / shared services
  PostgresConfig cfg_;
  std::vector<DeviceRegistryEntry> registry_;
  std::optional<SiteConfig> site_;
  SignalHandler &handler_;
  std::shared_ptr<spdlog::logger> postgresLogger_;

  // ------ producer/consumer queue
  mutable std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::queue<Event> queue_;
  std::size_t droppedSinceLastLog_{0};

  // ------ worker-thread-only state (no locking required)
  //
  // Keyed on the configured device name (= schema name). Each entry caches the
  // device descriptor, the cardinality/modal flags that decide which child
  // rows to write, and the schema-qualified SQL built once when the schema is
  // first set up. Caching the SQL (rather than preparing statements) keeps the
  // per-schema story simple and survives reconnects unchanged: the strings are
  // connection-independent, so a reconnect just replays the cached upserts with
  // no statements to re-register.

  struct CachedInverter {
    InverterTypes::Device device; // last upsert, replayed on reconnect
    bool isHybrid{false};
    int phases{0};
    int inputs{0};
    std::string upsertSql;
    std::string valuesSql;
    std::string phaseSql;
    std::string inputSql;
  };

  struct CachedMeter {
    MeterTypes::Device device;
    int phases{0};
    std::string upsertSql;
    std::string valuesSql;
    std::string phaseSql;
  };

  // libpq SQL trace sink. Declared before conn_ so reverse-order member
  // destruction tears the connection down first (PQfinish stops libpq writing
  // to the FILE*) and only then closes the file.
  struct FileCloser {
    void operator()(std::FILE *f) const noexcept {
      if (f)
        std::fclose(f);
    }
  };
  std::unique_ptr<std::FILE, FileCloser> sqlTraceFile_;

  std::unique_ptr<pg::Conn> conn_;
  std::unordered_map<std::string, CachedInverter> cachedInverters_;
  std::unordered_map<std::string, CachedMeter> cachedMeters_;
  bool extensionsChecked_{false};

  // ------ thread (must be last; joined in destructor)
  std::jthread worker_;
};

#endif /* POSTGRES_CLIENT_H_ */
