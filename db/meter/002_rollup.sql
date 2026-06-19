-- =============================================================================
-- fronius-bridge: per-device meter schema (migration 002)
--
-- The daily energy rollup: the derived `daily` table and the
-- compute_meter_energy() function that maintains it, kept together here and
-- apart from the raw-ingestion schema in 001. `daily` is written only by this
-- function, so its columns co-evolve with the function and the two belong in one
-- migration. A later migration can CREATE OR REPLACE the function without
-- touching 001.
--
-- ASCII only: this file is folded into the binary via #embed into a char
-- array, so any non-ASCII byte would break the build.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- Daily energy rollup table (one row per day).
--
-- One wide row per day holds this meter's active-energy import and export in
-- kWh, plus the coverage/continuity quality flags and the boundary readings the
-- rollup is derived from. Derived, not ingested: only compute_meter_energy()
-- (below) writes it, so it lives here with its function rather than in the
-- raw-ingestion schema (001). Plain table, not a hypertable: one tiny row per
-- day, queried by day.
--
-- The *_kwh_first/_kwh_last columns capture each counter's value at the day's
-- first and last sample (with first_ts/last_ts), so a later gap-estimator can
-- reconstruct cross-midnight deltas from this table alone even after the raw
-- samples age out under a retention policy.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS daily (
    day                DATE             PRIMARY KEY,
    imported_kwh       DOUBLE PRECISION,            -- NULL if not computable
    exported_kwh       DOUBLE PRECISION,            -- NULL if not computable
    coverage           DOUBLE PRECISION,            -- first..last span / day (fraction)
    complete           BOOLEAN          NOT NULL DEFAULT FALSE,  -- day fully measured
    continuity         DOUBLE PRECISION,            -- observed / expected 30s buckets (fraction)
    continuous         BOOLEAN          NOT NULL DEFAULT FALSE,  -- buckets densely cover the day
    sample_count       INTEGER,
    first_ts           TIMESTAMPTZ,                 -- time of first sample in day
    last_ts            TIMESTAMPTZ,                 -- time of last sample in day
    imported_kwh_first DOUBLE PRECISION,            -- import counter at first_ts
    imported_kwh_last  DOUBLE PRECISION,            -- import counter at last_ts
    exported_kwh_first DOUBLE PRECISION,            -- export counter at first_ts
    exported_kwh_last  DOUBLE PRECISION,            -- export counter at last_ts
    computed_at        TIMESTAMPTZ      NOT NULL DEFAULT now()
);

-- -----------------------------------------------------------------------------
-- compute_meter_energy(target_day, tz)
--
-- Computes this meter's daily active-energy import and export as the deltas of
-- the respective monotonic counters across the day and upserts one wide row into
-- daily. The day's first/last counter readings and their timestamps are stored
-- alongside the deltas, so a later gap-estimator can reconstruct cross-midnight
-- energy without rescanning (or retaining) the raw samples.
--
-- A daily total is a counter delta (last - first), so it is unaffected by gaps
-- *inside* the day; only missing data at the day's edges undercounts it.
-- coverage (the first..last span as a fraction of the day) measures exactly
-- that, so the row is flagged 'needs review' when coverage < v_coverage_min -- a
-- ~0.5% (about 7 min) tolerance to avoid false flags from normal edge slack. The
-- fraction's denominator is (day_end - day_start), which is DST-correct.
--
-- continuity is the gap-aware sibling of coverage: the share of the day's
-- expected 30-second buckets actually present. Unlike span-based coverage it
-- sees a mid-day outage, and it is the flag the simulated site rollup relies on,
-- since that estimate's self-consumption split is summed from these same buckets
-- and a mid-day gap would bias it low. complete and continuous diverge exactly
-- there: a full span keeps complete true while the missing buckets pull
-- continuous false.
--
-- Returns the day, the imported/exported kWh and a notes string (NULL when the
-- day is clean, else a short 'needs review' diagnostic carrying the coverage
-- figures). coverage, continuity and the two flags are stored on the row.
--
-- SET search_path FROM CURRENT binds the function to this device's schema at
-- creation time (search_path is '<schema>, public', so samples/daily resolve
-- here and the timescaledb first()/last() aggregates resolve in public). tz
-- defaults to the session TimeZone; target_day is read as a wall-clock day in
-- that zone via ::TIMESTAMP AT TIME ZONE tz, which keeps 23h/25h DST days
-- correct (a plain date::timestamptz cast does not).
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION compute_meter_energy(
    target_day DATE,
    tz         TEXT DEFAULT current_setting('TimeZone')
)
RETURNS TABLE (out_day            DATE,
              out_imported_kwh    DOUBLE PRECISION,
              out_exported_kwh    DOUBLE PRECISION,
              out_notes           TEXT)
LANGUAGE plpgsql
SET search_path FROM CURRENT
AS $$
DECLARE
    day_start    TIMESTAMPTZ    := (target_day::TIMESTAMP AT TIME ZONE tz);
    day_end      TIMESTAMPTZ    := ((target_day + 1)::TIMESTAMP AT TIME ZONE tz);
    full_secs    INTEGER        := EXTRACT(EPOCH FROM (day_end - day_start))::INTEGER;
    -- Flag a day for review when its samples cover less than this fraction of
    -- the day (data missing at the start and/or end). ~0.5% (about 7 min) slack
    -- avoids false flags from normal first/last-sample offset at the edges.
    v_coverage_min   CONSTANT NUMERIC := 0.995;
    -- A day's 30-second buckets must cover at least this fraction of the whole
    -- day or it is flagged non-continuous -- a mid-day outage that span-based
    -- coverage cannot see, and which would under-count the simulated site
    -- rollup's bucket sums.
    v_continuity_min CONSTANT NUMERIC := 0.90;
    v_count      INTEGER;
    v_first_t    TIMESTAMPTZ;
    v_last_t     TIMESTAMPTZ;
    v_span       INTEGER;            -- first..last span in seconds
    v_coverage   DOUBLE PRECISION;   -- first..last span / day, in [0, 1]
    v_complete   BOOLEAN;            -- day fully measured (positive form of the flag)
    v_observed   INTEGER;            -- distinct 30s buckets present in the day
    v_continuity DOUBLE PRECISION;   -- observed / expected buckets (gap-aware coverage)
    v_continuous BOOLEAN;            -- buckets densely cover the day
    v_imp_first  DOUBLE PRECISION;
    v_imp_last   DOUBLE PRECISION;
    v_imp_val    DOUBLE PRECISION;
    v_exp_first  DOUBLE PRECISION;
    v_exp_last   DOUBLE PRECISION;
    v_exp_val    DOUBLE PRECISION;
    v_notes      TEXT;
BEGIN
    SELECT count(*), min(time), max(time)
      INTO v_count, v_first_t, v_last_t
      FROM samples
     WHERE time >= day_start AND time < day_end;

    v_span := CASE WHEN v_count > 0
                   THEN EXTRACT(EPOCH FROM (v_last_t - v_first_t))::INTEGER
                   ELSE 0 END;

    v_coverage := CASE
        WHEN v_count > 0 THEN v_span::DOUBLE PRECISION / full_secs
        ELSE 0
    END;

    -- Continuity: count the distinct 30-second buckets the day's samples fall
    -- into (the same windows power_30sec forms) and compare with the number
    -- expected across the whole day. Computed from samples directly, so it needs
    -- no continuous aggregate and works for any day still inside sample retention.
    SELECT count(DISTINCT time_bucket('30 seconds', time)) INTO v_observed
      FROM samples
     WHERE time >= day_start AND time < day_end;

    v_continuity := CASE
        WHEN v_count > 0 THEN v_observed * 30.0 / full_secs
        ELSE 0
    END;

    SELECT first(energy_active_import, time), last(energy_active_import, time)
      INTO v_imp_first, v_imp_last FROM samples
     WHERE time >= day_start AND time < day_end
       AND energy_active_import IS NOT NULL;

    SELECT first(energy_active_export, time), last(energy_active_export, time)
      INTO v_exp_first, v_exp_last FROM samples
     WHERE time >= day_start AND time < day_end
       AND energy_active_export IS NOT NULL;

    v_imp_val := CASE
        WHEN v_imp_first IS NULL OR v_imp_last IS NULL OR v_imp_last < v_imp_first
        THEN NULL ELSE v_imp_last - v_imp_first
    END;
    v_exp_val := CASE
        WHEN v_exp_first IS NULL OR v_exp_last IS NULL OR v_exp_last < v_exp_first
        THEN NULL ELSE v_exp_last - v_exp_first
    END;

    v_notes := CASE
        WHEN v_count = 0 THEN 'no samples for day'
        WHEN v_imp_val IS NULL OR v_exp_val IS NULL
            THEN 'counter unavailable or went backwards'
        WHEN v_coverage < v_coverage_min
            THEN format('needs review: coverage %s%% (%s/%s s)',
                        round(100.0 * v_coverage), v_span, full_secs)
        ELSE NULL
    END;

    -- complete is the positive form of the review flag, from the same inputs:
    -- both counters usable and coverage clears v_coverage_min. Drives the site
    -- rollup.
    v_complete := v_imp_val IS NOT NULL AND v_exp_val IS NOT NULL
                  AND v_coverage >= v_coverage_min;

    -- continuous mirrors complete but on continuity: the 30-second buckets
    -- densely cover the day. It depends only on bucket presence, not the
    -- counters. complete and continuous diverge exactly on a mid-day gap: a full
    -- span keeps complete true while the missing buckets pull continuous false.
    v_continuous := v_count > 0 AND v_continuity >= v_continuity_min;

    INSERT INTO daily AS d
        (day, imported_kwh, exported_kwh, coverage, complete,
         continuity, continuous, sample_count, first_ts, last_ts,
         imported_kwh_first, imported_kwh_last,
         exported_kwh_first, exported_kwh_last, computed_at)
    VALUES
        (target_day, v_imp_val, v_exp_val, v_coverage, v_complete,
         v_continuity, v_continuous, v_count, v_first_t, v_last_t,
         v_imp_first, v_imp_last,
         v_exp_first, v_exp_last, now())
    ON CONFLICT (day) DO UPDATE
        SET imported_kwh       = EXCLUDED.imported_kwh,
            exported_kwh       = EXCLUDED.exported_kwh,
            coverage           = EXCLUDED.coverage,
            complete           = EXCLUDED.complete,
            continuity         = EXCLUDED.continuity,
            continuous         = EXCLUDED.continuous,
            sample_count       = EXCLUDED.sample_count,
            first_ts           = EXCLUDED.first_ts,
            last_ts            = EXCLUDED.last_ts,
            imported_kwh_first = EXCLUDED.imported_kwh_first,
            imported_kwh_last  = EXCLUDED.imported_kwh_last,
            exported_kwh_first = EXCLUDED.exported_kwh_first,
            exported_kwh_last  = EXCLUDED.exported_kwh_last,
            computed_at        = EXCLUDED.computed_at;

    out_day          := target_day;
    out_imported_kwh := v_imp_val;
    out_exported_kwh := v_exp_val;
    out_notes        := v_notes;
    RETURN NEXT;
END;
$$;
