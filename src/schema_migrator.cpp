#include "schema_migrator.h"
#include "db_error.h"
#include "pg.h"
#include <array>
#include <expected>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// SchemaMigrator
// ---------------------------------------------------------------------------

namespace {

// Re-wrap a failed-migration DbError with which migration was being applied,
// preserving the SQLSTATE-derived severity. A dropped connection (PROTOCOL,
// detected inside the shim) is passed through untouched so the worker still
// treats it as a reconnect trigger rather than a fatal migration error.
DbError withMigrationContext(const DbError &err, const Migration &m) {
  if (err.kind == DbError::Kind::PROTOCOL)
    return err;
  if (err.sqlstate)
    return DbError::makeWithState(DbError::Kind::MIGRATION, *err.sqlstate,
                                  "migration {} '{}' failed: {}", m.version,
                                  m.name, err.message);
  return DbError::make(DbError::Kind::MIGRATION, "migration {} '{}' failed: {}",
                       m.version, m.name, err.message);
}

} // namespace

SchemaMigrator::SchemaMigrator(pg::Conn &conn) : conn_(conn) {
  logger_ = spdlog::get("postgres");
  if (!logger_)
    logger_ = spdlog::default_logger();
}

std::expected<void, DbError> SchemaMigrator::checkExtensions() {
  // Extensions that must exist *in this database*. Only timescaledb qualifies:
  // the per-device sample tables are hypertables and the rollup uses
  // first()/last(), all resolved in the bridge's own database.
  //
  // pg_cron is deliberately NOT checked here. It can be created in only one
  // database per cluster (cron.database_name) and is commonly kept in a
  // dedicated database, scheduling the rollup against this one via
  // cron.schedule_in_database(). The bridge never calls pg_cron itself, so
  // requiring it in this database would be wrong for that topology and is not
  // a reliable signal that the rollup is set up. See DEPLOYMENT.md.
  static constexpr std::array<const char *, 1> required{"timescaledb"};

  // Read-only probe: no transaction needed, each query runs in autocommit.
  for (const char *ext : required) {
    auto res = conn_.execParams("SELECT 1 FROM pg_extension WHERE extname = $1",
                                pg::Params{ext}, DbError::Kind::MIGRATION);
    if (!res)
      return std::unexpected(res.error());
    if (res->empty()) {
      return std::unexpected(DbError::make(
          DbError::Kind::MIGRATION,
          "required PostgreSQL extension '{}' is not installed; install it "
          "during database setup (CREATE EXTENSION {}; see DEPLOYMENT.md)",
          ext, ext));
    }
  }
  return {};
}

std::expected<void, DbError>
SchemaMigrator::validateMigrationList(std::span<const Migration> migrations) {
  if (migrations.empty()) {
    return std::unexpected(
        DbError::make(DbError::Kind::INTERNAL, "migration list is empty"));
  }

  for (std::size_t i = 0; i < migrations.size(); ++i) {
    if (migrations[i].version <= 0) {
      return std::unexpected(
          DbError::make(DbError::Kind::INTERNAL,
                        "migration version must be positive (got {} for '{}')",
                        migrations[i].version, migrations[i].name));
    }
    if (i > 0 && migrations[i].version <= migrations[i - 1].version) {
      return std::unexpected(
          DbError::make(DbError::Kind::INTERNAL,
                        "migration versions must be strictly increasing "
                        "({} '{}' -> {} '{}')",
                        migrations[i - 1].version, migrations[i - 1].name,
                        migrations[i].version, migrations[i].name));
    }
    if (migrations[i].sql.empty()) {
      return std::unexpected(DbError::make(
          DbError::Kind::INTERNAL, "migration {} '{}' has empty SQL body",
          migrations[i].version, migrations[i].name));
    }
  }
  return {};
}

std::expected<void, DbError> SchemaMigrator::acquireAdvisoryLock() {
  if (auto res = conn_.exec("SELECT pg_advisory_xact_lock(" +
                                std::to_string(advisoryLockKey) + ")",
                            DbError::Kind::MIGRATION);
      !res)
    return std::unexpected(res.error());
  return {};
}

std::expected<void, DbError>
SchemaMigrator::createSchema(std::string_view schemaName) {
  if (auto res = conn_.exec("CREATE SCHEMA IF NOT EXISTS " +
                                conn_.quoteName(schemaName),
                            DbError::Kind::MIGRATION);
      !res)
    return std::unexpected(res.error());
  return {};
}

std::expected<void, DbError>
SchemaMigrator::setSearchPath(std::string_view schemaName) {
  // SET LOCAL so the search_path is scoped to this transaction and reverts on
  // commit, never leaking to later operations on the shared connection. The
  // device schema comes first (its objects are preferred and new objects land
  // there); public follows so the timescaledb functions resolve. The rollup
  // function's SET search_path FROM CURRENT captures this exact value.
  if (auto res = conn_.exec("SET LOCAL search_path TO " +
                                conn_.quoteName(schemaName) + ", public",
                            DbError::Kind::MIGRATION);
      !res)
    return std::unexpected(res.error());
  return {};
}

std::expected<void, DbError> SchemaMigrator::ensureSchemaVersionTable() {
  // Created inside the device schema (search_path is already set). No
  // schema_name or kind column: the schema is the context and its kind is
  // fixed, so each row is just (version, description, applied_at).
  if (auto res = conn_.exec("CREATE TABLE IF NOT EXISTS schema_version ("
                            "  version     INT          PRIMARY KEY,"
                            "  description TEXT         NOT NULL,"
                            "  applied_at  TIMESTAMPTZ  NOT NULL DEFAULT now()"
                            ")",
                            DbError::Kind::MIGRATION);
      !res)
    return std::unexpected(res.error());
  return {};
}

std::expected<int, DbError> SchemaMigrator::currentVersion() {
  // Two steps on purpose: a single "CASE WHEN to_regclass(...) IS NULL THEN 0
  // ELSE (SELECT ... FROM schema_version) END" fails at PLAN time when the
  // table is absent (the planner resolves every relation up front, before the
  // CASE can short-circuit). verify() runs this against a schema that may
  // never have been migrated, so first probe existence via to_regclass (text
  // arg, always safe), then read the version only if the table is present.
  //
  // Both queries return exactly one row by construction (a scalar to_regclass
  // and a COALESCE(MAX(...)) aggregate), so reading row 0 is always valid.
  auto reg = conn_.exec("SELECT to_regclass('schema_version')",
                        DbError::Kind::MIGRATION);
  if (!reg)
    return std::unexpected(reg.error());
  if (reg->isNull(0, 0))
    return 0; // table absent: schema not set up yet

  auto row = conn_.exec("SELECT COALESCE(MAX(version), 0) FROM schema_version",
                        DbError::Kind::MIGRATION);
  if (!row)
    return std::unexpected(row.error());
  return row->asInt(0, 0);
}

std::expected<void, DbError> SchemaMigrator::applyOne(const Migration &m) {
  // The SQL body may contain multiple statements; the no-parameter exec uses
  // PQexec, which runs the whole body as one multi-statement command.
  if (auto res = conn_.exec(m.sql, DbError::Kind::MIGRATION); !res)
    return std::unexpected(withMigrationContext(res.error(), m));

  if (auto res = conn_.execParams(
          "INSERT INTO schema_version (version, description) VALUES ($1, $2)",
          pg::Params{m.version, std::string{m.name}}, DbError::Kind::MIGRATION);
      !res)
    return std::unexpected(withMigrationContext(res.error(), m));

  return {};
}

std::expected<void, DbError>
SchemaMigrator::migrate(std::span<const Migration> migrations,
                        std::string_view schemaName) {
  if (auto v = validateMigrationList(migrations); !v)
    return v;

  const int highestEmbedded = migrations.back().version;

  // The advisory xact lock, the SET LOCAL search_path, and the DDL must all run
  // in one transaction; the guard rolls back on any early return below.
  auto tx = pg::Transaction::begin(conn_, DbError::Kind::MIGRATION);
  if (!tx)
    return std::unexpected(tx.error());

  if (auto r = acquireAdvisoryLock(); !r)
    return r;
  if (auto r = createSchema(schemaName); !r)
    return r;
  if (auto r = setSearchPath(schemaName); !r)
    return r;
  if (auto r = ensureSchemaVersionTable(); !r)
    return r;

  auto currentResult = currentVersion();
  if (!currentResult)
    return std::unexpected(currentResult.error());
  const int current = *currentResult;

  if (current == highestEmbedded) {
    logger_->info("Schema '{}' is up to date (version {})", schemaName,
                  current);
    if (auto committed = tx->commit(DbError::Kind::MIGRATION); !committed)
      return std::unexpected(committed.error());
    return {};
  }

  if (current > highestEmbedded) {
    logger_->warn("Schema '{}' (version {}) is newer than this binary supports "
                  "(version {}); leaving as-is",
                  schemaName, current, highestEmbedded);
    if (auto committed = tx->commit(DbError::Kind::MIGRATION); !committed)
      return std::unexpected(committed.error());
    return {};
  }

  logger_->info("Migrating schema '{}' from version {} to {}", schemaName,
                current, highestEmbedded);

  for (const auto &m : migrations) {
    if (m.version <= current)
      continue;

    logger_->info("Applying migration {} '{}' to '{}'", m.version, m.name,
                  schemaName);
    if (auto r = applyOne(m); !r)
      return r;
  }

  if (auto committed = tx->commit(DbError::Kind::MIGRATION); !committed)
    return std::unexpected(committed.error());

  logger_->info("Schema '{}' migration complete (now at version {})",
                schemaName, highestEmbedded);
  return {};
}

std::expected<void, DbError>
SchemaMigrator::verify(std::span<const Migration> migrations,
                       std::string_view schemaName) {
  if (auto v = validateMigrationList(migrations); !v)
    return v;

  const int highestEmbedded = migrations.back().version;

  // The advisory xact lock and SET LOCAL search_path are transaction-scoped, so
  // even the read-only verify path runs inside a transaction.
  auto tx = pg::Transaction::begin(conn_, DbError::Kind::MIGRATION);
  if (!tx)
    return std::unexpected(tx.error());

  if (auto r = acquireAdvisoryLock(); !r)
    return r;
  // verify() writes nothing: no CREATE SCHEMA, no schema_version table. Just
  // point search_path at the (possibly absent) schema and read the version.
  if (auto r = setSearchPath(schemaName); !r)
    return r;

  auto currentResult = currentVersion();
  if (!currentResult)
    return std::unexpected(currentResult.error());
  const int current = *currentResult;

  if (auto committed = tx->commit(DbError::Kind::MIGRATION); !committed)
    return std::unexpected(committed.error());

  if (current == highestEmbedded) {
    logger_->info("Schema '{}' is up to date (version {})", schemaName,
                  current);
    return {};
  }

  if (current > highestEmbedded) {
    logger_->warn("Schema '{}' (version {}) is newer than this binary supports "
                  "(version {}); proceeding",
                  schemaName, current, highestEmbedded);
    return {};
  }

  return std::unexpected(
      DbError::make(DbError::Kind::MIGRATION,
                    "schema '{}' is at version {} but version {} is required; "
                    "rerun without --no-migrate to create/upgrade it",
                    schemaName, current, highestEmbedded));
}
