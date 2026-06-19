-- =============================================================================
-- fronius-bridge: shared public schema (migration 001)
--
-- The device registry: one row per configured device, written by the bridge at
-- startup. The site-level energy functions (later migrations) read it to learn
-- each device's role -- which schema is production, which meter is the
-- grid/feed-in reference, which meters are consumption -- without hardcoding
-- device names. A device's schema is named after the device, so device_name
-- doubles as the schema to query (e.g. format('%I.daily', device_name)).
--
-- Unlike the per-device schemas, public already exists; the migrator creates
-- the schema_version ledger here and applies this track once per database.
--
-- ASCII only: this file is folded into the binary via #embed.
-- =============================================================================

CREATE TABLE IF NOT EXISTS device_registry (
    device_name TEXT        PRIMARY KEY,   -- also the device's schema name
    kind        TEXT        NOT NULL,      -- 'inverter' or 'meter'
    location    TEXT,                      -- 'feed-in' | 'consumption' | NULL
    is_primary  BOOLEAN     NOT NULL DEFAULT FALSE,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT device_registry_kind_chk
        CHECK (kind IN ('inverter', 'meter')),
    CONSTRAINT device_registry_location_chk
        CHECK (location IS NULL OR location IN ('feed-in', 'consumption'))
);
