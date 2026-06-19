-- =============================================================================
-- fronius-bridge: per-device meter schema (migration 003)
--
-- Two continuous aggregates of active power (total across phases). Each carries
-- the per-bucket average plus the minimum and maximum, so the dashboard can
-- draw a min/max envelope around the average power line:
--
--   power_5min   5-minute buckets -- long-range power plots.
--   power_30sec  30-second buckets -- recent high-resolution plots and the
--                high-fidelity source for the simulated self-consumption
--                calculation (finer buckets give a tighter per-bucket
--                min(production, consumption) overlap).
--
-- Compression and retention for both aggregates are set in migration 004,
-- alongside the raw hypertables, so the whole storage budget lives in one place.
--
-- Both are created WITH NO DATA on purpose. A continuous aggregate created WITH
-- DATA (the default) triggers an immediate refresh, and TimescaleDB refuses to
-- run that refresh inside a transaction block -- but the migrator applies every
-- pending migration inside one transaction. WITH NO DATA skips the initial
-- refresh, so this migration is transaction-safe; the refresh policies below
-- materialise the buckets from then on. refresh_continuous_aggregate() must
-- never be called here for the same reason. To backfill history once (e.g.
-- after importing old samples), run it manually outside a transaction:
--   CALL <schema>.refresh_continuous_aggregate('power_5min', NULL, NULL);
--
-- The views are created unconditionally (no IF NOT EXISTS): the migrator applies
-- each migration exactly once, in a transaction that rolls back on any error.
--
-- ASCII only: this file is folded into the binary via #embed.
-- =============================================================================

-- 5-minute buckets: long-range power plots.
CREATE MATERIALIZED VIEW power_5min
WITH (timescaledb.continuous) AS
SELECT time_bucket('5 minutes', time) AS bucket,
       avg(power_active)              AS avg_power,  -- W, total active power
       min(power_active)              AS min_power,
       max(power_active)              AS max_power
FROM samples
GROUP BY bucket
WITH NO DATA;

-- Each run (re)materialises [now - start_offset, now - end_offset]: start_offset
-- re-covers late-arriving samples; end_offset leaves the still-filling recent
-- buckets to real-time aggregation. if_not_exists keeps re-application harmless.
SELECT add_continuous_aggregate_policy('power_5min',
    start_offset      => INTERVAL '2 hours',
    end_offset        => INTERVAL '10 minutes',
    schedule_interval => INTERVAL '5 minutes',
    if_not_exists     => TRUE);

-- 30-second buckets: recent high-resolution plots and the simulation source.
CREATE MATERIALIZED VIEW power_30sec
WITH (timescaledb.continuous) AS
SELECT time_bucket('30 seconds', time) AS bucket,
       avg(power_active)               AS avg_power,  -- W, total active power
       min(power_active)               AS min_power,
       max(power_active)               AS max_power
FROM samples
GROUP BY bucket
WITH NO DATA;

-- Refresh every minute so the recent high-resolution buckets stay current.
SELECT add_continuous_aggregate_policy('power_30sec',
    start_offset      => INTERVAL '1 hour',
    end_offset        => INTERVAL '1 minute',
    schedule_interval => INTERVAL '1 minute',
    if_not_exists     => TRUE);
