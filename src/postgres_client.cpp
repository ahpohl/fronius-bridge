#include "postgres_client.h"
#include "db_error.h"
#include "migrations.h"
#include "pg.h"
#include "schema_migrator.h"
#include "signal_handler.h"
#include "utils.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Threshold for batched drop logging: avoid a per-drop warn() flood during a
// sustained DB outage, but a once-per-N notice is informative.
constexpr std::size_t dropLogThreshold = 100;

// Convert Values::time (epoch milliseconds, UTC) to an ISO-8601 timestamp
// string ("YYYY-MM-DD HH:MM:SS+00") for binding as TIMESTAMPTZ. The epoch is
// already UTC, so the instant is exact; PostgreSQL converts to the session
// time zone on display. Millisecond precision matches the source.
std::string timeFromMillis(uint64_t ms) {
  const auto tp = std::chrono::sys_time<std::chrono::milliseconds>{
      std::chrono::milliseconds{ms}};
  return std::format("{:%Y-%m-%d %H:%M:%S}+00", tp);
}

// libpq notice receiver: route server NOTICE/WARNING messages through the
// postgres logger instead of libpq's default sink (a raw write to stderr that
// bypasses spdlog and interleaves with it). Keyed on the non-localized severity
// field so the routing is locale-independent: WARNING -> warn, everything
// quieter -> debug, which keeps routine setup chatter out of the default info
// output. arg is the postgres logger, which outlives the connection.
void routeNotice(void *arg, const PGresult *res) {
  auto *logger = static_cast<spdlog::logger *>(arg);
  if (!logger || !res)
    return;

  const char *severity = PQresultErrorField(res, PG_DIAG_SEVERITY_NONLOCALIZED);
  const char *primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
  const std::string_view message =
      primary ? std::string_view{primary}
              : DbError::firstLine(PQresultErrorMessage(res));

  if (severity && std::string_view{severity} == "WARNING")
    logger->warn("{}", message);
  else
    logger->debug("{}", message);
}

} // namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

PostgresClient::PostgresClient(const PostgresConfig &cfg,
                               std::vector<DeviceRegistryEntry> registry,
                               std::optional<SiteConfig> site,
                               SignalHandler &signalHandler)
    : cfg_(cfg), registry_(std::move(registry)), site_(std::move(site)),
      handler_(signalHandler) {
  // Synchronous validation only. The connection is established lazily by the
  // worker so a transient DB outage at startup does not block the bridge;
  // reconnect-delay bounds were already checked by parseReconnectDelay().
  if (cfg_.dsn.empty())
    throw std::invalid_argument("postgres.dsn is empty");

  postgresLogger_ = spdlog::get("postgres");
  if (!postgresLogger_)
    postgresLogger_ = spdlog::default_logger();

  // Start the worker last - all members are valid and `this` is safe to
  // observe from another thread.
  worker_ = std::jthread{&PostgresClient::run, this};
}

PostgresClient::~PostgresClient() {
  // Destruction, not the shutdown signal, retires the worker: main destroys
  // every master first, and their destructors emit a final "disconnected" that
  // has to reach the database. Clearing the flag under the queue mutex closes
  // the window where the worker evaluates a wait predicate and sleeps just
  // after we notify.
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    active_.store(false);
  }
  queueCv_.notify_all();
  postgresLogger_->debug("PostgresClient shut down");
}

// ---------------------------------------------------------------------------
// Producer side: callbacks -> queue
// ---------------------------------------------------------------------------

void PostgresClient::onInverterDevice(std::string deviceName,
                                      InverterTypes::Device dev) {
  enqueue(
      Event{.deviceName = std::move(deviceName), .payload = std::move(dev)});
}

void PostgresClient::onMeterDevice(std::string deviceName,
                                   MeterTypes::Device dev) {
  enqueue(
      Event{.deviceName = std::move(deviceName), .payload = std::move(dev)});
}

void PostgresClient::onInverter(std::string deviceName,
                                InverterTypes::Values values) {
  enqueue(
      Event{.deviceName = std::move(deviceName), .payload = std::move(values)});
}

void PostgresClient::onMeter(std::string deviceName,
                             MeterTypes::Values values) {
  enqueue(
      Event{.deviceName = std::move(deviceName), .payload = std::move(values)});
}

void PostgresClient::onAvailability(std::string deviceName, bool online) {
  // Stamped on the producer thread, the same way the masters stamp
  // Values::time, so the recorded instant is when the device was seen to change
  // state and not when the worker got to it.
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  enqueue(Event{.deviceName = std::move(deviceName),
                .payload = Availability{.online = online, .time = now}});
}

void PostgresClient::enqueue(Event ev) {
  std::size_t droppedToReport = 0;

  {
    std::lock_guard<std::mutex> lock(queueMutex_);

    // Bounded FIFO: when full, drop the oldest event to make room. Newer
    // telemetry is more valuable and TimescaleDB compresses runs of similar
    // values cheaply, so the gap is comparatively cheap.
    if (queue_.size() >= cfg_.queueSize) {
      queue_.pop();
      ++droppedSinceLastLog_;

      if (droppedSinceLastLog_ >= dropLogThreshold)
        droppedToReport = std::exchange(droppedSinceLastLog_, 0);
    }

    queue_.push(std::move(ev));
  }

  queueCv_.notify_one();

  if (droppedToReport > 0) {
    postgresLogger_->warn(
        "Postgres queue full: dropped {} oldest events since last log",
        droppedToReport);
  }
}

// ---------------------------------------------------------------------------
// Worker side: queue -> database
// ---------------------------------------------------------------------------

void PostgresClient::run() {
  postgresLogger_->debug("Postgres worker thread started");

  const std::chrono::seconds minDelay{cfg_.reconnectDelay.min};
  const std::chrono::seconds maxDelay{cfg_.reconnectDelay.max};
  const bool exponential = cfg_.reconnectDelay.exponential;

  std::chrono::seconds backoff{minDelay};

  // Set when a FATAL error has already escalated to handler_.shutdown(): the
  // outer loop keys on active_, which only the destructor clears, so it needs
  // its own reason to stop reconnecting.
  bool fatal = false;

  while (active_.load()) {

    // --- Connect, check extensions, replay cached device upserts. ---
    auto setup = connectAndPrepare();
    if (!setup) {
      const auto &err = setup.error();
      if (err.severity == DbError::Severity::FATAL) {
        // Configuration / schema / privilege / missing-extension error:
        // retrying produces the same failure forever. Escalate to shutdown so
        // the operator gets a non-zero exit and a chance to fix the cause.
        postgresLogger_->error("Postgres setup failed: {}", err.describe());
        handler_.shutdown(true, "Postgres setup failed");
        fatal = true;
        break;
      }
      postgresLogger_->warn("Postgres setup failed: {} - retrying in {}s",
                            err.describe(), backoff.count());
      sleepBackoff(backoff);
      backoff = exponential ? std::min(backoff * 2, maxDelay) : minDelay;
      continue;
    }

    backoff = minDelay;
    postgresLogger_->info("Postgres connected");

    // --- Drain the queue until the destructor asks us to stop, or until a
    //     connection-level failure. The head deliberately does not test
    //     active_ on its own: the break below needs the queue empty too, so a
    //     stop drains the backlog instead of discarding it. This terminates
    //     because every producer is destroyed before we are. ---
    while (true) {
      Event ev;
      {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait(lock, [&] { return !queue_.empty() || !active_.load(); });
        if (!active_.load() && queue_.empty())
          break;
        ev = std::move(queue_.front());
        queue_.pop();
      }

      auto result = processEvent(ev);
      if (!result) {
        const auto &err = result.error();

        if (err.severity == DbError::Severity::FATAL) {
          postgresLogger_->error("Postgres event failed: {}", err.describe());
          handler_.shutdown(true, "Postgres write failed");
          fatal = true;
          break;
        }

        if (err.kind == DbError::Kind::PROTOCOL) {
          // Connection broken: drop out of the inner loop and reconnect. The
          // caches persist, so the reconnect replays the device upserts.
          conn_.reset();
          break;
        }

        // An IDENTITY rejection quarantines its own device and leaves the
        // rest writing. The upsert already logged it once, with the device
        // named and without repeating on every later event, so saying so a
        // second time here adds nothing.
        if (err.kind == DbError::Kind::IDENTITY)
          continue;

        // TRANSIENT QUERY: warn and continue; the event is lost. Includes a
        // duplicate-timestamp unique violation (a benign re-poll) and a
        // constraint violation (a data/schema mismatch worth noticing but not
        // worth taking the whole bridge down for).
        postgresLogger_->warn("Postgres event failed: {}", err.describe());
      }
    }

    if (fatal)
      break;
  }

  // Reaching here with a non-empty queue means the events could not be
  // written: Postgres was down at shutdown, or the connection broke during the
  // final drain. Say how many were lost rather than dropping them silently.
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (!queue_.empty())
      postgresLogger_->warn("Postgres worker stopping with {} undelivered "
                            "events; they are discarded",
                            queue_.size());
  }

  postgresLogger_->debug("Postgres worker thread stopping");

  if (conn_)
    conn_->close();
}

void PostgresClient::sleepBackoff(std::chrono::seconds duration) {
  std::unique_lock<std::mutex> lock(queueMutex_);
  queueCv_.wait_for(lock, duration, [&] { return !active_.load(); });
}

void PostgresClient::attachSqlTrace() {
  if (!sqlTraceFile_) {
    std::error_code ec;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);

    if (ec || temp_dir.empty()) {
      postgresLogger_->debug(
          "temp_directory_path failed: {}, falling back to current_path",
          ec.message());
      temp_dir = std::filesystem::current_path();
    }

    const std::filesystem::path path =
        temp_dir / std::format("fronius-bridge-libpq-{}.trace",
                               static_cast<long>(::getpid()));

    // Append mode preserves wire-level history across reconnects. fopen failure
    // is non-fatal: tracing is a diagnostic aid, not a correctness requirement.
    sqlTraceFile_.reset(std::fopen(path.c_str(), "a"));
    if (!sqlTraceFile_) {
      const int err = errno;
      postgresLogger_->warn(
          "Could not open SQL trace file '{}': {} - SQL tracing disabled",
          path.string(), std::strerror(err));
      return;
    }

    std::setvbuf(sqlTraceFile_.get(), nullptr, _IOLBF, 0);
    postgresLogger_->trace("libpq SQL trace -> {}", path.string());
  }

  if (conn_)
    conn_->trace(sqlTraceFile_.get());
}

std::expected<void, DbError> PostgresClient::connectAndPrepare() {
  // --- Open or reopen the connection. PQconnectdb does not throw: failure is
  //     reported via isOpen()/connectError(). The dead handle is released
  //     before returning so it is not held across the backoff. ---
  conn_ = std::make_unique<pg::Conn>(cfg_.dsn);
  if (!conn_->isOpen()) {
    DbError err = conn_->connectError();
    conn_.reset();
    return std::unexpected(err);
  }

  // --- Route server NOTICE/WARNING messages through our logger. Installed
  //     here so it covers the migrations that follow, and re-installed on
  //     every reconnect since each new PGconn starts with the default. ---
  conn_->setNoticeReceiver(&routeNotice, postgresLogger_.get());

  // --- Hook libpq's wire-level SQL trace if requested by log level. ---
  if (postgresLogger_->should_log(spdlog::level::trace))
    attachSqlTrace();

  // --- Verify the required extensions exist (once per process; the check is
  //     database-global). A missing extension is FATAL and shuts down. ---
  if (!extensionsChecked_) {
    SchemaMigrator m{*conn_};
    if (auto r = m.checkExtensions(); !r)
      return r;
    // Bring the shared public schema (device registry, site-energy objects)
    // up to date once per process, the same lifecycle as the extension check.
    // Honors --no-migrate like the per-device path.
    auto pub = cfg_.autoMigrate ? m.migrate(publicMigrations, "public")
                                : m.verify(publicMigrations, "public");
    if (!pub)
      return pub;
    // Reflect the configured device roster into public.device_registry. Done
    // after the public schema exists and before the flag is set, so a transient
    // failure here retries the whole one-time block on the next reconnect.
    if (auto r = syncRegistry(); !r)
      return r;
    extensionsChecked_ = true;
  }

  // --- Replay device upserts from the cache. On a reconnect the schemas
  //     already exist, so this just refreshes each device row and last_seen;
  //     the cached SQL is connection-independent and needs no re-preparing. On
  //     first connect the caches are empty and the device callbacks populate
  //     them via the normal lazy-migrate path. `device` is copied out before
  //     the call so the upsert does not read the entry it rewrites.
  //
  //     Quarantined devices are skipped: `device` holds the last *accepted*
  //     identity, so replaying it would pass the check and lift the quarantine
  //     while the wrong hardware is still connected.
  //
  //     Offline devices are skipped because the upsert stamps last_seen, which
  //     would claim we heard from a device that has not answered since it
  //     dropped. A reconnecting master's device re-read restores the row
  //     through the normal path. ---
  for (const auto &[name, cached] : cachedInverters_) {
    if (cached.identityRejected || isKnownOffline(name))
      continue;
    auto device = cached.device;
    if (auto r = upsertInverterDevice(name, device); !r)
      return r;
  }
  for (const auto &[name, cached] : cachedMeters_) {
    if (cached.identityRejected || isKnownOffline(name))
      continue;
    auto device = cached.device;
    if (auto r = upsertMeterDevice(name, device); !r)
      return r;
  }

  return {};
}

std::expected<void, DbError> PostgresClient::processEvent(const Event &ev) {
  return std::visit(
      [this, &ev](const auto &payload) -> std::expected<void, DbError> {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, InverterTypes::Device>) {
          return upsertInverterDevice(ev.deviceName, payload);
        } else if constexpr (std::is_same_v<T, MeterTypes::Device>) {
          return upsertMeterDevice(ev.deviceName, payload);
        } else if constexpr (std::is_same_v<T, InverterTypes::Values>) {
          return insertInverterValues(ev.deviceName, payload);
        } else if constexpr (std::is_same_v<T, MeterTypes::Values>) {
          return insertMeterValues(ev.deviceName, payload);
        } else if constexpr (std::is_same_v<T, Availability>) {
          return updateAvailability(ev.deviceName, payload);
        }
      },
      ev.payload);
}

std::expected<void, DbError> PostgresClient::syncRegistry() {
  auto tx = pg::Transaction::begin(*conn_, DbError::Kind::MIGRATION);
  if (!tx)
    return std::unexpected(tx.error());

  // now() is transaction_timestamp(): one value for the whole transaction, so
  // every row upserted below shares an updated_at and the reconciling DELETE
  // removes exactly the strictly-older rows left untouched this run -- the
  // devices no longer configured. Avoids binding the name list as an array
  // just to delete its complement.
  for (const auto &e : registry_) {
    if (auto r = conn_->execParams(
            "INSERT INTO public.device_registry "
            "(device_name, kind, location, is_primary, updated_at) "
            "VALUES ($1, $2, $3, $4, now()) "
            "ON CONFLICT (device_name) DO UPDATE SET "
            "kind = EXCLUDED.kind, location = EXCLUDED.location, "
            "is_primary = EXCLUDED.is_primary, updated_at = now()",
            pg::Params{e.name, e.kind, e.location, e.primary},
            DbError::Kind::MIGRATION);
        !r)
      return std::unexpected(r.error());
  }

  if (auto r = conn_->exec(
          "DELETE FROM public.device_registry WHERE updated_at < now()",
          DbError::Kind::MIGRATION);
      !r)
    return std::unexpected(r.error());

  // Single-row site location (NULL when not configured). Blind upsert: the
  // boolean PK pinned to TRUE means there is only ever one row. Written every
  // run so removing the `site:` section clears it.
  const std::optional<double> lat =
      site_ ? std::optional<double>{site_->latitude} : std::nullopt;
  const std::optional<double> lon =
      site_ ? std::optional<double>{site_->longitude} : std::nullopt;
  const std::optional<double> hor =
      site_ ? std::optional<double>{site_->horizon} : std::nullopt;
  if (auto r = conn_->execParams(
          "INSERT INTO public.site (id, latitude, longitude, horizon_deg) "
          "VALUES (TRUE, $1, $2, $3) "
          "ON CONFLICT (id) DO UPDATE SET "
          "latitude = EXCLUDED.latitude, longitude = EXCLUDED.longitude, "
          "horizon_deg = EXCLUDED.horizon_deg, updated_at = now()",
          pg::Params{lat, lon, hor}, DbError::Kind::MIGRATION);
      !r)
    return std::unexpected(r.error());

  if (auto committed = tx->commit(DbError::Kind::MIGRATION); !committed)
    return std::unexpected(committed.error());
  return {};
}

// ---------------------------------------------------------------------------
// SQL paths
// ---------------------------------------------------------------------------

std::expected<void, DbError>
PostgresClient::upsertInverterDevice(const std::string &name,
                                     const InverterTypes::Device &dev) {
  if (!conn_)
    return std::unexpected(DbError::make(
        DbError::Kind::INTERNAL, "upsertInverterDevice without a connection"));

  auto it = cachedInverters_.find(name);
  if (it == cachedInverters_.end()) {
    // First sight of this device: create/verify its schema, then build the
    // schema-qualified SQL it will reuse. Not repeated on reconnect (the entry
    // stays cached).
    SchemaMigrator m{*conn_};
    auto migrated = cfg_.autoMigrate ? m.migrate(inverterMigrations, name)
                                     : m.verify(inverterMigrations, name);
    if (!migrated)
      return std::unexpected(migrated.error());

    const std::string s = conn_->quoteName(name);
    CachedInverter ci;
    ci.upsertSql =
        "INSERT INTO " + s +
        ".device (serial_number, manufacturer, model, firmware_version, "
        "data_manager, register_model, inverter_id, slave_id, hybrid, "
        "mppt_tracker, phases, power_rating, last_seen) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12, now()) "
        "ON CONFLICT (serial_number) DO UPDATE SET "
        "manufacturer=EXCLUDED.manufacturer, model=EXCLUDED.model, "
        "firmware_version=EXCLUDED.firmware_version, "
        "data_manager=EXCLUDED.data_manager, "
        "register_model=EXCLUDED.register_model, "
        "inverter_id=EXCLUDED.inverter_id, slave_id=EXCLUDED.slave_id, "
        "hybrid=EXCLUDED.hybrid, mppt_tracker=EXCLUDED.mppt_tracker, "
        "phases=EXCLUDED.phases, power_rating=EXCLUDED.power_rating, "
        "last_seen=now()";
    ci.touchSql = "UPDATE " + s + ".device SET last_seen=$1";
    ci.valuesSql =
        "INSERT INTO " + s +
        ".samples (time, ac_energy, ac_power_active, ac_power_apparent, "
        "ac_power_reactive, ac_power_factor, ac_frequency, dc_power, "
        "efficiency) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)";
    ci.phaseSql = "INSERT INTO " + s +
                  ".phase_samples (time, phase_id, ac_voltage, ac_current) "
                  "VALUES ($1,$2,$3,$4)";
    ci.inputSql = "INSERT INTO " + s +
                  ".input_samples (time, input_id, dc_voltage, dc_current, "
                  "dc_power, dc_energy) VALUES ($1,$2,$3,$4,$5,$6)";
    it = cachedInverters_.emplace(name, std::move(ci)).first;
  }

  // After the migration (the table must exist) and before the upsert (which
  // would otherwise refresh last_seen on a row this device does not own).
  if (auto ok = checkDeviceIdentity(name, dev.serialNumber); !ok) {
    // Log on entry only; see upsertMeterDevice for the rationale.
    if (!it->second.identityRejected) {
      postgresLogger_->warn("Quarantining inverter '{}': {}", name,
                            ok.error().message);
      it->second.identityRejected = true;
    }
    return std::unexpected(ok.error());
  }

  if (it->second.identityRejected) {
    postgresLogger_->info(
        "Inverter '{}' identity accepted again, resuming writes", name);
    it->second.identityRejected = false;
  }

  // A single ON CONFLICT upsert is atomic on its own, so it runs in autocommit
  // with no surrounding transaction.
  if (auto r = conn_->execParams(
          it->second.upsertSql,
          pg::Params{dev.serialNumber, dev.manufacturer, dev.model,
                     dev.fwVersion, dev.dataManagerVersion, dev.registerModel,
                     dev.id, dev.slaveID, dev.isHybrid, dev.inputs, dev.phases,
                     static_cast<float>(dev.acPowerApparent)});
      !r)
    return std::unexpected(r.error());

  auto &ci = it->second;
  ci.device = dev;
  ci.isHybrid = dev.isHybrid;
  ci.phases = dev.phases;
  ci.inputs = dev.inputs;

  postgresLogger_->debug("Upserted inverter '{}' (serial '{}')", name,
                         dev.serialNumber);
  return {};
}

std::expected<void, DbError>
PostgresClient::checkDeviceIdentity(const std::string &name,
                                    const std::string &serial) {
  // A serial we cannot trust is worse than no serial: it would be written as
  // the baseline and every later comparison would agree with it. A transport
  // error already fails loudly upstream, so a degenerate value here means the
  // device answered with a valid frame and bad content.
  if (serial.empty() || serial == "0")
    return std::unexpected(DbError::make(
        DbError::Kind::IDENTITY, "unusable serial number '{}'", serial));

  auto res = conn_->execParams("SELECT serial_number FROM " +
                                   conn_->quoteName(name) + ".device",
                               pg::Params{});
  if (!res)
    return std::unexpected(res.error());

  // No row yet: first run against this schema. The upsert that follows writes
  // the baseline every later run is compared against.
  if (res->empty())
    return {};

  // More than one row means the singleton index is missing or was dropped by
  // hand. Refuse rather than pick one to compare against.
  if (res->rows() > 1)
    return std::unexpected(DbError::make(
        DbError::Kind::IDENTITY, "schema holds {} device rows, expected one",
        res->rows()));

  const std::string stored = res->value(0, 0);
  if (stored == serial)
    return {};

  return std::unexpected(
      DbError::make(DbError::Kind::IDENTITY,
                    "schema belongs to serial '{}' but the device reports '{}'",
                    stored, serial));
}

std::expected<void, DbError>
PostgresClient::upsertMeterDevice(const std::string &name,
                                  const MeterTypes::Device &dev) {
  if (!conn_)
    return std::unexpected(DbError::make(
        DbError::Kind::INTERNAL, "upsertMeterDevice without a connection"));

  auto it = cachedMeters_.find(name);
  if (it == cachedMeters_.end()) {
    SchemaMigrator m{*conn_};
    auto migrated = cfg_.autoMigrate ? m.migrate(meterMigrations, name)
                                     : m.verify(meterMigrations, name);
    if (!migrated)
      return std::unexpected(migrated.error());

    const std::string s = conn_->quoteName(name);
    CachedMeter cm;
    cm.upsertSql =
        "INSERT INTO " + s +
        ".device (serial_number, manufacturer, model, firmware_version, "
        "register_model, meter_id, slave_id, phases, last_seen) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8, now()) "
        "ON CONFLICT (serial_number) DO UPDATE SET "
        "manufacturer=EXCLUDED.manufacturer, model=EXCLUDED.model, "
        "firmware_version=EXCLUDED.firmware_version, "
        "register_model=EXCLUDED.register_model, meter_id=EXCLUDED.meter_id, "
        "slave_id=EXCLUDED.slave_id, phases=EXCLUDED.phases, last_seen=now()";
    cm.touchSql = "UPDATE " + s + ".device SET last_seen=$1";
    cm.valuesSql =
        "INSERT INTO " + s +
        ".samples (time, energy_active_import, energy_active_export, "
        "energy_apparent_import, energy_apparent_export, "
        "energy_reactive_import, energy_reactive_export, power_active, "
        "power_apparent, power_reactive, power_factor, frequency, voltage_ph, "
        "voltage_pp, current) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15)";
    cm.phaseSql =
        "INSERT INTO " + s +
        ".phase_samples (time, phase_id, power_active, power_apparent, "
        "power_reactive, power_factor, voltage_ph, voltage_pp, current) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)";
    it = cachedMeters_.emplace(name, std::move(cm)).first;
  }

  // After the migration (the table must exist) and before the upsert (which
  // would otherwise refresh last_seen on a row this device does not own).
  if (auto ok = checkDeviceIdentity(name, dev.serialNumber); !ok) {
    // Log on entry only: the sample drops that follow repeat at the poll rate,
    // and one actionable line beats thousands the operator has to filter.
    if (!it->second.identityRejected) {
      postgresLogger_->warn("Quarantining meter '{}': {}", name,
                            ok.error().message);
      it->second.identityRejected = true;
    }
    return std::unexpected(ok.error());
  }

  if (it->second.identityRejected) {
    postgresLogger_->info("Meter '{}' identity accepted again, resuming writes",
                          name);
    it->second.identityRejected = false;
  }

  // A non-SunSpec meter (e.g. the EBZ over SML) leaves the SunSpec/Modbus
  // identity fields unset; store NULL rather than empty/zero so the schema's
  // value checks only see real values.
  const std::optional<std::string> registerModel =
      dev.registerModel.empty() ? std::nullopt
                                : std::optional<std::string>{dev.registerModel};
  const std::optional<int> meterId =
      dev.id > 0 ? std::optional<int>{dev.id} : std::nullopt;
  const std::optional<int> slaveId =
      dev.slaveID > 0 ? std::optional<int>{dev.slaveID} : std::nullopt;

  // A single ON CONFLICT upsert is atomic on its own, so it runs in autocommit
  // with no surrounding transaction.
  if (auto r =
          conn_->execParams(it->second.upsertSql,
                            pg::Params{dev.serialNumber, dev.manufacturer,
                                       dev.model, dev.fwVersion, registerModel,
                                       meterId, slaveId, dev.phases});
      !r)
    return std::unexpected(r.error());

  auto &cm = it->second;
  cm.device = dev;
  cm.phases = dev.phases;

  postgresLogger_->debug("Upserted meter '{}' (serial '{}')", name,
                         dev.serialNumber);
  return {};
}

std::expected<void, DbError>
PostgresClient::updateAvailability(const std::string &name,
                                   const Availability &av) {
  // Advance the state first: the edge is what matters, and it must be consumed
  // even when the stamp below is skipped, or a "disconnected" that slipped past
  // the master's gate would stamp again with a later time.
  const bool wasOnline = std::exchange(deviceOnline_[name], av.online);
  if (av.online || !wasOnline)
    return {};

  if (!conn_)
    return std::unexpected(DbError::make(
        DbError::Kind::INTERNAL, "updateAvailability without a connection"));

  // Only a device that reached an accepted upsert this run has a row to stamp:
  // no cache entry means no schema yet, and a quarantined entry belongs to
  // other hardware whose last_seen is not ours to move.
  const std::string *touchSql = nullptr;
  if (auto it = cachedInverters_.find(name);
      it != cachedInverters_.end() && !it->second.identityRejected)
    touchSql = &it->second.touchSql;
  else if (auto mt = cachedMeters_.find(name);
           mt != cachedMeters_.end() && !mt->second.identityRejected)
    touchSql = &mt->second.touchSql;

  if (!touchSql)
    return {};

  // Unqualified UPDATE: device_singleton_idx holds every per-device table to
  // at most one row, so there is nothing to select between. A missing row
  // updates nothing, the right outcome for a device that disconnected before
  // its first upsert landed.
  //
  // The bound instant is the producer's, not the server's now(): that keeps the
  // column honest across a database outage and puts device.last_seen on the
  // same clock as samples.time.
  const auto ts = timeFromMillis(av.time);
  if (auto r = conn_->execParams(*touchSql, pg::Params{ts}); !r)
    return std::unexpected(r.error());

  postgresLogger_->debug("Stamped last_seen for '{}' on disconnect", name);
  return {};
}

bool PostgresClient::isKnownOffline(const std::string &name) const {
  // find() rather than operator[]: an absent entry means no transition has been
  // processed yet, which is not the same as offline. Answering true would hold
  // last_seen back for a device that is in fact up.
  auto it = deviceOnline_.find(name);
  return it != deviceOnline_.end() && !it->second;
}

std::expected<void, DbError>
PostgresClient::insertInverterValues(const std::string &name,
                                     const InverterTypes::Values &v) {
  if (!conn_)
    return std::unexpected(DbError::make(
        DbError::Kind::INTERNAL, "insertInverterValues without a connection"));

  const auto it = cachedInverters_.find(name);
  if (it == cachedInverters_.end()) {
    // Values can briefly precede the device upsert at startup. Without the
    // cache there is neither schema SQL nor cardinality flags, so drop it.
    return std::unexpected(DbError::make(
        DbError::Kind::QUERY,
        "inverter '{}' values arrived before its device upsert, dropping",
        name));
  }

  const auto &cache = it->second;

  // See insertMeterValues: a quarantined schema takes no samples.
  if (cache.identityRejected) {
    postgresLogger_->debug("Inverter '{}' is quarantined, dropping sample",
                           name);
    return {};
  }

  const auto ts = timeFromMillis(v.time);
  const bool isHybrid = cache.isHybrid;
  const int phases = std::clamp(cache.phases, 1, 3);
  const int inputs = std::clamp(cache.inputs, 1, 2);

  auto tx = pg::Transaction::begin(*conn_);
  if (!tx)
    return std::unexpected(tx.error());

  if (auto r = conn_->execParams(
          cache.valuesSql, pg::Params{ts, Utils::scaleToKilo(v.acEnergy),
                                      static_cast<float>(v.acPowerActive),
                                      static_cast<float>(v.acPowerApparent),
                                      static_cast<float>(v.acPowerReactive),
                                      static_cast<float>(v.acPowerFactor),
                                      static_cast<float>(v.acFrequency),
                                      static_cast<float>(v.dcPower),
                                      static_cast<float>(v.efficiency)});
      !r)
    return std::unexpected(r.error());

  const std::array<const InverterTypes::Phase *, 3> phaseList = {
      &v.phase1, &v.phase2, &v.phase3};
  for (int i = 0; i < phases; ++i) {
    if (auto r = conn_->execParams(
            cache.phaseSql,
            pg::Params{ts, static_cast<int16_t>(i + 1),
                       static_cast<float>(phaseList[i]->acVoltage),
                       static_cast<float>(phaseList[i]->acCurrent)});
        !r)
      return std::unexpected(r.error());
  }

  const std::array<const InverterTypes::Input *, 2> inputList = {&v.input1,
                                                                 &v.input2};
  for (int i = 0; i < inputs; ++i) {
    // dc_energy is NULL for hybrid inverters (the struct holds 0.0 as a
    // sentinel that would corrupt monotonic-counter analysis); otherwise
    // scale raw Wh to kWh on the way to the column.
    std::optional<double> dcEnergy =
        isHybrid
            ? std::nullopt
            : std::optional<double>{Utils::scaleToKilo(inputList[i]->dcEnergy)};

    if (auto r = conn_->execParams(
            cache.inputSql,
            pg::Params{ts, static_cast<int16_t>(i + 1),
                       static_cast<float>(inputList[i]->dcVoltage),
                       static_cast<float>(inputList[i]->dcCurrent),
                       static_cast<float>(inputList[i]->dcPower), dcEnergy});
        !r)
      return std::unexpected(r.error());
  }

  if (auto committed = tx->commit(); !committed)
    return std::unexpected(committed.error());
  return {};
}

std::expected<void, DbError>
PostgresClient::insertMeterValues(const std::string &name,
                                  const MeterTypes::Values &v) {
  if (!conn_)
    return std::unexpected(DbError::make(
        DbError::Kind::INTERNAL, "insertMeterValues without a connection"));

  const auto it = cachedMeters_.find(name);
  if (it == cachedMeters_.end()) {
    return std::unexpected(DbError::make(
        DbError::Kind::QUERY,
        "meter '{}' values arrived before its device upsert, dropping", name));
  }

  const auto &cache = it->second;

  // The schema belongs to other hardware: writing these samples would append
  // one device's readings to another's counter series, the damage the identity
  // check exists to prevent. Already reported when the quarantine was entered,
  // so drop quietly rather than logging at the poll rate.
  if (cache.identityRejected) {
    postgresLogger_->debug("Meter '{}' is quarantined, dropping sample", name);
    return {};
  }

  const auto ts = timeFromMillis(v.time);
  const int phases = std::clamp(cache.phases, 1, 3);

  auto tx = pg::Transaction::begin(*conn_);
  if (!tx)
    return std::unexpected(tx.error());

  // Energies are scaled Wh -> kWh at bind time, the same boundary scaling
  // libfronius applies for the MQTT JSON.
  if (auto r = conn_->execParams(
          cache.valuesSql,
          pg::Params{
              ts, Utils::scaleToKilo(v.activeEnergyImport),
              Utils::scaleToKilo(v.activeEnergyExport),
              Utils::scaleToKilo(v.apparentEnergyImport),
              Utils::scaleToKilo(v.apparentEnergyExport),
              Utils::scaleToKilo(v.reactiveEnergyImport),
              Utils::scaleToKilo(v.reactiveEnergyExport),
              static_cast<float>(v.activePower),
              static_cast<float>(v.apparentPower),
              static_cast<float>(v.reactivePower),
              static_cast<float>(v.powerFactor),
              static_cast<float>(v.frequency), static_cast<float>(v.phVoltage),
              static_cast<float>(v.ppVoltage), static_cast<float>(v.current)});
      !r)
    return std::unexpected(r.error());

  const std::array<const MeterTypes::Phase *, 3> phaseList = {
      &v.phase1, &v.phase2, &v.phase3};
  for (int i = 0; i < phases; ++i) {
    if (auto r = conn_->execParams(
            cache.phaseSql,
            pg::Params{ts, static_cast<int16_t>(i + 1),
                       static_cast<float>(phaseList[i]->activePower),
                       static_cast<float>(phaseList[i]->apparentPower),
                       static_cast<float>(phaseList[i]->reactivePower),
                       static_cast<float>(phaseList[i]->powerFactor),
                       static_cast<float>(phaseList[i]->phVoltage),
                       static_cast<float>(phaseList[i]->ppVoltage),
                       static_cast<float>(phaseList[i]->current)});
        !r)
      return std::unexpected(r.error());
  }

  if (auto committed = tx->commit(); !committed)
    return std::unexpected(committed.error());
  return {};
}
