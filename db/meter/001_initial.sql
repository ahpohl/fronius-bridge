-- =============================================================================
-- fronius-bridge: per-device meter schema (migration 001)
--
-- Applied by the schema migrator into one schema per configured meter, named
-- after the device (e.g. "grid", "heatpump"). The migrator sets search_path to
-- the target schema before running this file, so every object is created
-- unqualified inside that schema. Covers both Fronius SunSpec meters and the
-- EBZ Easymeter: both feed the same MeterTypes::Values shape, so one template
-- serves both. Cross-device totals are computed on the fly by summing across
-- schemas (see DEPLOYMENT.md).
--
-- Energy unit convention: energy columns are kWh-family
-- (kWh active, kVAh apparent, kvarh reactive). The in-memory Values structs
-- hold raw Wh/VAh/varh; the consumer scales by 1e-3 (Utils::scaleToKilo) at
-- insert time, the same boundary scaling used for MQTT. Powers are W/VA/var,
-- voltages V, currents A, frequency Hz.
--
-- TimescaleDB: the per-sample tables are hypertables. The timescaledb
-- extension must already exist on the database (CREATE EXTENSION is a
-- privileged operator step, see DEPLOYMENT.md); the bridge verifies the
-- extension is present at startup and refuses to run without it.
--
-- ASCII only: this file is folded into the binary via #embed into a char
-- array, so any non-ASCII byte would break the build.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- Device metadata. Keyed on the immutable hardware serial; the consumer
-- upserts on serial_number, refreshing last_seen and mutable fields.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS device (
    serial_number    TEXT         PRIMARY KEY,
    manufacturer     TEXT         NOT NULL,
    model            TEXT         NOT NULL,
    firmware_version TEXT,
    -- SunSpec/Modbus identity. Nullable: a non-SunSpec meter (e.g. an EBZ
    -- Easymeter read over SML/OBIS) has no register model, SunSpec model id, or
    -- Modbus slave address, and stores NULL for these.
    register_model   TEXT,
    meter_id         INTEGER,                 -- SunSpec model id (e.g. 203, 213)
    slave_id         INTEGER,
    phases           SMALLINT     NOT NULL,   -- 1, 2, or 3
    first_seen       TIMESTAMPTZ  NOT NULL DEFAULT now(),
    last_seen        TIMESTAMPTZ  NOT NULL DEFAULT now(),

    CONSTRAINT device_phases_chk   CHECK (phases BETWEEN 1 AND 3),
    -- The slave_id / register_model checks validate only real values; a CHECK
    -- passes when its expression is NULL, so NULL (not applicable) is accepted.
    CONSTRAINT device_slave_id_chk CHECK (slave_id IS NULL OR slave_id BETWEEN 1 AND 247),
    CONSTRAINT device_register_chk CHECK (register_model IN ('int+sf', 'float'))
);

-- -----------------------------------------------------------------------------
-- Per-sample top-level values. Energy columns are monotonic counters.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS samples (
    time                   TIMESTAMPTZ      NOT NULL,
    energy_active_import   DOUBLE PRECISION,   -- kWh, monotonic counter
    energy_active_export   DOUBLE PRECISION,   -- kWh
    energy_apparent_import DOUBLE PRECISION,   -- kVAh
    energy_apparent_export DOUBLE PRECISION,   -- kVAh
    energy_reactive_import DOUBLE PRECISION,   -- kvarh
    energy_reactive_export DOUBLE PRECISION,   -- kvarh
    power_active           REAL,               -- W (sum across phases)
    power_apparent         REAL,               -- VA
    power_reactive         REAL,               -- var
    power_factor           REAL,
    frequency              REAL,               -- Hz
    voltage_ph             REAL,               -- V (phase-to-neutral)
    voltage_pp             REAL,               -- V (phase-to-phase)
    current                REAL,               -- A (sum across phases)

    -- One reading per instant. Includes the hypertable partition column
    -- (time), as TimescaleDB requires of any unique index.
    CONSTRAINT samples_time_uniq UNIQUE (time)
);

SELECT create_hypertable('samples', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS samples_time_idx ON samples (time DESC);

-- -----------------------------------------------------------------------------
-- Per-phase values.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS phase_samples (
    time           TIMESTAMPTZ NOT NULL,
    phase_id       SMALLINT    NOT NULL,
    power_active   REAL,                        -- W
    power_apparent REAL,                        -- VA
    power_reactive REAL,                        -- var
    power_factor   REAL,
    voltage_ph     REAL,                        -- V (phase-to-neutral)
    voltage_pp     REAL,                        -- V (phase-to-phase, this->next)
    current        REAL,                        -- A

    CONSTRAINT phase_samples_phase_chk CHECK (phase_id BETWEEN 1 AND 3),
    -- One reading per phase per instant (includes the partition column time).
    CONSTRAINT phase_samples_time_phase_uniq UNIQUE (time, phase_id)
);

SELECT create_hypertable('phase_samples', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS phase_samples_phase_time_idx
    ON phase_samples (phase_id, time DESC);
