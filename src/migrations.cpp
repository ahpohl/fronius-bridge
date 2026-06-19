#include "migrations.h"
#include "schema_migrator.h"
#include <array>
#include <span>
#include <string_view>

// ---------------------------------------------------------------------------
// migrations.cpp
//
// Materializes embedded SQL bodies via #embed (P1967) and builds the per-kind
// registries that SchemaMigrator consumes. The bytes live in exactly one
// translation unit; everywhere else sees only the spans.
//
// The trailing ',0' on each #embed array guarantees NUL termination so the
// bytes can be wrapped in a string_view safely (size - 1 drops the NUL).
//
// SQL files are required to be pure ASCII so the bytes fit a signed 'char'
// array without -Wnarrowing. Non-ASCII text (em dashes, smart quotes, accented
// characters) must be transliterated to ASCII before embedding.
//
// The #embed paths are resolved relative to the project root via the
// --embed-dir flag set in CMakeLists.txt.
// ---------------------------------------------------------------------------

namespace {

constexpr char inverter001[] = {
#embed "db/inverter/001_initial.sql"
    , 0};

constexpr char meter001[] = {
#embed "db/meter/001_initial.sql"
    , 0};

constexpr char inverter002[] = {
#embed "db/inverter/002_rollup.sql"
    , 0};

constexpr char meter002[] = {
#embed "db/meter/002_rollup.sql"
    , 0};

constexpr char inverter003[] = {
#embed "db/inverter/003_power_agg.sql"
    , 0};

constexpr char inverter004[] = {
#embed "db/inverter/004_retention.sql"
    , 0};

constexpr char meter003[] = {
#embed "db/meter/003_power_agg.sql"
    , 0};

constexpr char meter004[] = {
#embed "db/meter/004_retention.sql"
    , 0};

constexpr char public001[] = {
#embed "db/public/001_registry.sql"
    , 0};

constexpr char public002[] = {
#embed "db/public/002_site_energy.sql"
    , 0};

constexpr char public003[] = {
#embed "db/public/003_site_rollup.sql"
    , 0};

constexpr char public004[] = {
#embed "db/public/004_site_location.sql"
    , 0};

constexpr std::array inverterArray = {
    Migration{1, "initial",
              std::string_view{inverter001, sizeof(inverter001) - 1}},
    Migration{2, "rollup",
              std::string_view{inverter002, sizeof(inverter002) - 1}},
    Migration{3, "power_agg",
              std::string_view{inverter003, sizeof(inverter003) - 1}},
    Migration{4, "retention",
              std::string_view{inverter004, sizeof(inverter004) - 1}},
};

constexpr std::array meterArray = {
    Migration{1, "initial", std::string_view{meter001, sizeof(meter001) - 1}},
    Migration{2, "rollup", std::string_view{meter002, sizeof(meter002) - 1}},
    Migration{3, "power_agg", std::string_view{meter003, sizeof(meter003) - 1}},
    Migration{4, "retention", std::string_view{meter004, sizeof(meter004) - 1}},
};

constexpr std::array publicArray = {
    Migration{1, "registry",
              std::string_view{public001, sizeof(public001) - 1}},
    Migration{2, "site_energy",
              std::string_view{public002, sizeof(public002) - 1}},
    Migration{3, "site_rollup",
              std::string_view{public003, sizeof(public003) - 1}},
    Migration{4, "site_location",
              std::string_view{public004, sizeof(public004) - 1}},
};

} // namespace

const std::span<const Migration> inverterMigrations{inverterArray};
const std::span<const Migration> meterMigrations{meterArray};
const std::span<const Migration> publicMigrations{publicArray};
