-- =============================================================================
-- fronius-bridge: per-device inverter schema (migration 001)
--
-- Applied by the schema migrator into one schema per configured inverter,
-- named after the device (e.g. "primo"). The migrator sets search_path to the
-- target schema before running this file, so every object below is created
-- unqualified inside that schema. Each inverter's tables, its daily rollup, and
-- its rollup function all live together in its own schema; the schema name is
-- the device's identity. A shared public.device_registry records each device's
-- role and location for the site rollup, but holds no per-device state here.
-- Cross-device totals are computed on the fly by
-- summing across schemas (see DEPLOYMENT.md).
--
-- Energy unit convention: energy columns are kWh-family (kWh here). The
-- in-memory Values structs hold raw Wh (SunSpec register semantics); the
-- consumer scales by 1e-3 (Utils::scaleToKilo) at insert time, the same
-- boundary scaling used for MQTT. Powers are W/VA/var, voltages V, currents A,
-- frequency Hz, efficiency percent.
--
-- TimescaleDB: the per-sample tables are hypertables. The timescaledb
-- extension must already exist on the database (CREATE EXTENSION is a
-- privileged operator step, see DEPLOYMENT.md); the bridge verifies the
-- extension is present at startup and refuses to run without it. Creating the
-- hypertables themselves needs no elevated privileges.
--
-- ASCII only: this file is folded into the binary via #embed into a char
-- array, so any non-ASCII byte would break the build. No em-dashes, no
-- degree/non-ASCII symbols in comments.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- Device metadata. Keyed on the immutable hardware serial. Replacing the
-- physical device (new serial, same config name) inserts a new row; old rows
-- are retained for provenance. The consumer upserts on serial_number,
-- refreshing last_seen and the mutable descriptive fields.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS device (
    serial_number    TEXT         PRIMARY KEY,
    manufacturer     TEXT         NOT NULL,
    model            TEXT         NOT NULL,
    firmware_version TEXT,
    data_manager     TEXT,
    register_model   TEXT         NOT NULL,
    inverter_id      INTEGER      NOT NULL,   -- SunSpec model id (e.g. 101, 103)
    slave_id         INTEGER      NOT NULL,
    hybrid           BOOLEAN      NOT NULL,
    mppt_tracker     SMALLINT     NOT NULL,   -- number of DC inputs (1 or 2)
    phases           SMALLINT     NOT NULL,   -- 1, 2, or 3
    power_rating     REAL         NOT NULL,   -- VA (apparent power rating)
    first_seen       TIMESTAMPTZ  NOT NULL DEFAULT now(),
    last_seen        TIMESTAMPTZ  NOT NULL DEFAULT now(),

    CONSTRAINT device_phases_chk   CHECK (phases BETWEEN 1 AND 3),
    CONSTRAINT device_mppt_chk     CHECK (mppt_tracker BETWEEN 1 AND 2),
    CONSTRAINT device_slave_id_chk CHECK (slave_id BETWEEN 1 AND 247),
    CONSTRAINT device_register_chk CHECK (register_model IN ('int+sf', 'float'))
);

-- -----------------------------------------------------------------------------
-- Per-sample top-level AC values. Energy is a monotonic lifetime counter.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS samples (
    time              TIMESTAMPTZ      NOT NULL,
    ac_energy         DOUBLE PRECISION,   -- kWh, monotonic lifetime counter
    ac_power_active   REAL,               -- W
    ac_power_apparent REAL,               -- VA
    ac_power_reactive REAL,               -- var
    ac_power_factor   REAL,
    ac_frequency      REAL,               -- Hz
    dc_power          REAL,               -- W (total across inputs)
    efficiency        REAL,               -- percent (computed)

    -- One reading per instant. Includes the hypertable partition column
    -- (time), as TimescaleDB requires of any unique index.
    CONSTRAINT samples_time_uniq UNIQUE (time)
);

SELECT create_hypertable('samples', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS samples_time_idx ON samples (time DESC);

-- -----------------------------------------------------------------------------
-- Per-phase AC values.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS phase_samples (
    time       TIMESTAMPTZ NOT NULL,
    phase_id   SMALLINT    NOT NULL,
    ac_voltage REAL,                       -- V
    ac_current REAL,                       -- A

    CONSTRAINT phase_samples_phase_chk CHECK (phase_id BETWEEN 1 AND 3),
    -- One reading per phase per instant (includes the partition column time).
    CONSTRAINT phase_samples_time_phase_uniq UNIQUE (time, phase_id)
);

SELECT create_hypertable('phase_samples', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS phase_samples_phase_time_idx
    ON phase_samples (phase_id, time DESC);

-- -----------------------------------------------------------------------------
-- Per-DC-input values. dc_energy is NULL on hybrid inverters (not exposed).
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS input_samples (
    time       TIMESTAMPTZ      NOT NULL,
    input_id   SMALLINT         NOT NULL,
    dc_voltage REAL,                       -- V
    dc_current REAL,                       -- A
    dc_power   REAL,                       -- W
    dc_energy  DOUBLE PRECISION,           -- kWh, NULL on hybrid inverters

    CONSTRAINT input_samples_input_chk CHECK (input_id BETWEEN 1 AND 2),
    -- One reading per input per instant (includes the partition column time).
    CONSTRAINT input_samples_time_input_uniq UNIQUE (time, input_id)
);

SELECT create_hypertable('input_samples', 'time', if_not_exists => TRUE);

CREATE INDEX IF NOT EXISTS input_samples_input_time_idx
    ON input_samples (input_id, time DESC);
