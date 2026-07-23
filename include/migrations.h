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
// Each track is independently numbered. SchemaMigrator applies the two device
// tracks into each device's own schema, and the public track once per database
// into the pre-existing public schema (device registry, site-level objects):
//   inverter -> inverterMigrations  (db/inverter/NNN_*.sql)
//   meter    -> meterMigrations     (db/meter/NNN_*.sql)
//   public   -> publicMigrations    (db/public/NNN_*.sql)
//
// To add a migration: drop NNN_<name>.sql into db/<track>/ (NNN consecutive
// within the track) and append a Migration entry to that track's array in
// migrations.cpp.
// ---------------------------------------------------------------------------

extern const std::span<const Migration> inverterMigrations;
extern const std::span<const Migration> meterMigrations;
extern const std::span<const Migration> publicMigrations;

#endif /* MIGRATIONS_H_ */
