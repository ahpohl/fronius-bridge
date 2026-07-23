#ifndef SCHEMA_MIGRATOR_H_
#define SCHEMA_MIGRATOR_H_

#include "db_error.h"
#include <expected>
#include <memory>
#include <span>
#include <spdlog/logger.h>
#include <string_view>

// Forward-declared so this header stays free of <libpq-fe.h>: the connection is
// taken by reference and only schema_migrator.cpp needs the complete type.
// migrations.{h,cpp} include this header solely for the Migration struct.
namespace pg {
class Conn;
} // namespace pg

// ---------------------------------------------------------------------------
// Migration
//
// One row in a per-kind migration registry. The `sql` field is a view onto the
// embedded SQL body; ownership lives in migrations.cpp where the bytes are
// materialized via #embed. `version` is consecutive within the kind.
// ---------------------------------------------------------------------------

struct Migration {
  int version;
  std::string_view name;
  std::string_view sql;
};

// ---------------------------------------------------------------------------
// SchemaMigrator
//
// Applies (or verifies) embedded SQL schema migrations into one PostgreSQL
// schema per device. The migrator creates the schema, sets the search_path to
// '<schema>, public', and tracks applied versions in a schema-local
// `schema_version` ledger. There is no central registry: each schema carries
// its own gapless 1..N history, fed from the matching per-kind registry.
//
// All work for one schema runs in a single transaction gated by an advisory
// lock, so two fronius-bridge instances starting against the same database
// cannot race and a partially-migrated schema is impossible.
//
// The search_path puts the device schema first, so its own objects are
// preferred and new objects land there, and public second, so the timescaledb
// functions (create_hypertable, first()/last()) resolve. The rollup function's
// SET search_path FROM CURRENT captures exactly this.
// ---------------------------------------------------------------------------

class SchemaMigrator {
public:
  explicit SchemaMigrator(pg::Conn &conn);

  // Verify the extension the schemas depend on *in this database* is installed
  // (timescaledb, for the hypertables and the first()/last() aggregates).
  // FATAL if missing -- installing it is a privileged operator step (see
  // DEPLOYMENT.md). Database-scoped, so call once per connection.
  std::expected<void, DbError> checkExtensions();

  // Apply pending migrations into `schemaName`, creating the schema if needed.
  // Idempotent: a no-op when the schema is already at the registry's highest
  // version. Each applied migration is recorded in <schemaName>.schema_version.
  std::expected<void, DbError> migrate(std::span<const Migration> migrations,
                                       std::string_view schemaName);

  // Verify `schemaName` is at the registry's highest version without writing
  // anything. Fails if the schema is missing or behind (the operator must rerun
  // without --no-migrate); warns if it is ahead.
  std::expected<void, DbError> verify(std::span<const Migration> migrations,
                                      std::string_view schemaName);

private:
  // Constant identifier for pg_advisory_xact_lock(int8). Hex-ASCII for
  // "FRONIUSB" - recognizable in pg_locks and unlikely to collide.
  static constexpr int64_t advisoryLockKey = 0x46524F4E49555342LL;

  std::expected<void, DbError> acquireAdvisoryLock();
  std::expected<void, DbError> createSchema(std::string_view schemaName);
  std::expected<void, DbError> setSearchPath(std::string_view schemaName);
  std::expected<void, DbError> ensureSchemaVersionTable();
  std::expected<int, DbError> currentVersion();
  std::expected<void, DbError> applyOne(const Migration &m);

  // Validate that the migration list is monotonic and well-formed. Run once
  // per migrate()/verify() call.
  static std::expected<void, DbError>
  validateMigrationList(std::span<const Migration> migrations);

  pg::Conn &conn_;
  std::shared_ptr<spdlog::logger> logger_;
};

#endif /* SCHEMA_MIGRATOR_H_ */
