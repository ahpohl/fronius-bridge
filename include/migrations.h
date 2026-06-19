#ifndef MIGRATIONS_H_
#define MIGRATIONS_H_

#include "schema_migrator.h"
#include <span>

// ---------------------------------------------------------------------------
// migrations.h
//
// Declares the per-kind registries of embedded SQL migrations. The byte data
// and the arrays live in migrations.cpp; this header exposes only spans so
// callers do not pay the compile-time cost of the #embed directives.
//
// Each device kind has its own independently-numbered registry. SchemaMigrator
// applies the matching one into each device's schema:
//   inverter -> inverterMigrations  (db/inverter/NNN_*.sql)
//   meter    -> meterMigrations     (db/meter/NNN_*.sql)
//
// The public track is applied once per database into the pre-existing public
// schema, for site-level objects shared across devices (the device registry,
// and later the site-energy table):
//   public   -> publicMigrations    (db/public/NNN_*.sql)
//
// To add a migration for a track:
//   1. Drop NNN_<name>.sql into db/<track>/ (NNN consecutive within the track)
//   2. Append a Migration entry to the track's array in migrations.cpp
// ---------------------------------------------------------------------------

extern const std::span<const Migration> inverterMigrations;
extern const std::span<const Migration> meterMigrations;
extern const std::span<const Migration> publicMigrations;

#endif /* MIGRATIONS_H_ */
