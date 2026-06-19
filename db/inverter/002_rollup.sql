-- =============================================================================
-- fronius-bridge: per-device inverter schema (migration 002)
--
-- The daily energy rollup: the derived `daily` table and the
-- compute_inverter_energy() function that maintains it, kept together here and
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
-- One row per day holds this inverter's produced energy in kWh, plus the
-- coverage/continuity quality flags and the boundary readings the rollup is
-- derived from. Derived, not ingested: only compute_inverter_energy() (below)
-- writes it, so it lives here with its function rather than in the raw-ingestion
-- schema (001). Plain table, not a hypertable: one tiny row per day, queried by
-- day.
--
-- first_kwh/last_kwh capture the ac_energy counter at the day's first and last
-- sample (with first_ts/last_ts), so a later gap-estimator can reconstruct
-- cross-midnight deltas from this table alone even after the raw samples age out
-- under a retention policy.
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS daily (
    day              DATE             PRIMARY KEY,
    produced_kwh     DOUBLE PRECISION,            -- NULL if not computable
    coverage         DOUBLE PRECISION,            -- first..last span / daylight (fraction)
    complete         BOOLEAN          NOT NULL DEFAULT FALSE,  -- day fully measured
    continuity       DOUBLE PRECISION,            -- observed / expected 30s buckets (fraction)
    continuous       BOOLEAN          NOT NULL DEFAULT FALSE,  -- buckets densely cover daylight
    sample_count     INTEGER,
    first_ts         TIMESTAMPTZ,                 -- time of first sample in day
    last_ts          TIMESTAMPTZ,                 -- time of last sample in day
    first_kwh        DOUBLE PRECISION,            -- ac_energy counter at first_ts
    last_kwh         DOUBLE PRECISION,            -- ac_energy counter at last_ts
    computed_at      TIMESTAMPTZ      NOT NULL DEFAULT now()
);

-- -----------------------------------------------------------------------------
-- compute_inverter_energy(target_day, tz)
--
-- Computes this inverter's daily production as the delta of the ac_energy
-- counter across the day and upserts one row into daily. The day's first/last
-- counter readings and their timestamps are stored alongside the delta, so a
-- later gap-estimator can reconstruct cross-midnight energy without rescanning
-- (or retaining) the raw samples.
--
-- A daily total is a counter delta (last - first), so it is unaffected by gaps
-- *inside* the day. An inverter produces only in daylight and its SunSpec
-- interface is down at night, so coverage is measured against the day's
-- available daylight, not the 24h day: solar_daylight() (public, fed lat/long
-- and the horizon angle from public.site) gives the expected daylight seconds,
-- and coverage is the first..last span as a fraction of that. A clear day sits
-- at ~1.0; coverage < v_coverage_min is flagged 'needs review', as is a
-- whole-day outage (zero production). Coverage is not capped: the SunSpec
-- interface reports a little outside geometric daylight, so a clear day landing
-- just over 1.0 is the signal to lower the site horizon (public.site.horizon_deg)
-- to match. When the site has no coordinates, daylight is unknown: coverage
-- falls back to the span as a fraction of the whole day (DST-correct) and only
-- the zero-production flag applies.
--
-- continuity is the gap-aware sibling of coverage: the share of the expected
-- 30-second buckets actually present. Unlike span-based coverage it sees a
-- mid-day outage, and it is the flag the simulated site rollup relies on, since
-- that estimate's self-consumption split is summed from these same buckets and a
-- mid-day gap would bias it low. complete and continuous diverge exactly there:
-- a full span keeps complete true while the missing buckets pull continuous false.
--
-- Returns the day, the produced kWh and a notes string (NULL when the day is
-- clean, else a short 'needs review' diagnostic carrying the coverage figures).
-- coverage, continuity and the two flags are stored on the row.
--
-- SET search_path FROM CURRENT binds the function to this device's schema at
-- creation time, so the unqualified samples/daily references resolve in this
-- schema and the timescaledb first()/last() aggregates resolve in public. tz
-- defaults to the session TimeZone; target_day is read as a wall-clock day in
-- that zone via ::TIMESTAMP AT TIME ZONE tz, keeping 23h/25h DST days correct.
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION compute_inverter_energy(
    target_day DATE,
    tz         TEXT DEFAULT current_setting('TimeZone')
)
RETURNS TABLE (out_day          DATE,
              out_produced_kwh  DOUBLE PRECISION,
              out_notes         TEXT)
LANGUAGE plpgsql
SET search_path FROM CURRENT
AS $$
DECLARE
    day_start  TIMESTAMPTZ    := (target_day::TIMESTAMP AT TIME ZONE tz);
    day_end    TIMESTAMPTZ    := ((target_day + 1)::TIMESTAMP AT TIME ZONE tz);
    full_secs  INTEGER        := EXTRACT(EPOCH FROM (day_end - day_start))::INTEGER;
    -- Flag a day for review when its first..last span covers less than this
    -- fraction of the expected daylight window (data missing at dawn and/or
    -- dusk). 0.90 allows the handful of edge buckets the inverter misses because
    -- SunSpec is not up for the full geometric daylight.
    v_coverage_min   CONSTANT NUMERIC := 0.90;
    -- A day's 30-second buckets must cover at least this fraction of the expected
    -- daylight window or the day is flagged non-continuous -- a mid-day outage
    -- that span-based coverage cannot see.
    v_continuity_min CONSTANT NUMERIC := 0.90;
    v_count    INTEGER;
    v_first_t  TIMESTAMPTZ;
    v_last_t   TIMESTAMPTZ;
    v_first    DOUBLE PRECISION;   -- ac_energy counter at first sample
    v_last     DOUBLE PRECISION;   -- ac_energy counter at last sample
    v_span     INTEGER;            -- first..last span in seconds
    v_lat      DOUBLE PRECISION;   -- site coords, for the daylight denominator
    v_lon      DOUBLE PRECISION;
    v_horizon  DOUBLE PRECISION;   -- site horizon angle for solar_daylight
    v_daylight INTEGER;            -- expected daylight seconds; NULL if no coords
    v_coverage DOUBLE PRECISION;   -- span / daylight (or / day when no coords)
    v_complete BOOLEAN;            -- day fully measured (positive form of the flag)
    v_observed   INTEGER;          -- distinct 30s buckets present in the day
    v_continuity DOUBLE PRECISION; -- observed / expected buckets (gap-aware coverage)
    v_continuous BOOLEAN;          -- buckets densely cover the expected window
    v_produced DOUBLE PRECISION;   -- daily production (last - first counter)
    v_notes    TEXT;
BEGIN
    SELECT count(*), min(time), max(time),
           first(ac_energy, time), last(ac_energy, time)
      INTO v_count, v_first_t, v_last_t, v_first, v_last
      FROM samples
     WHERE time >= day_start AND time < day_end
       AND ac_energy IS NOT NULL;

    v_span := CASE WHEN v_count > 0
                   THEN EXTRACT(EPOCH FROM (v_last_t - v_first_t))::INTEGER
                   ELSE 0 END;

    -- Expected daylight for the day, from the site location and horizon. NULL
    -- (no coords) or 0 (polar night) means we cannot judge against daylight.
    SELECT latitude, longitude, horizon_deg INTO v_lat, v_lon, v_horizon
      FROM public.site;
    IF v_lat IS NOT NULL AND v_lon IS NOT NULL THEN
        SELECT seconds INTO v_daylight
          FROM public.solar_daylight(v_lat, v_lon, target_day, tz, v_horizon);
    END IF;

    -- Coverage against daylight when known (the meaningful denominator for a
    -- daylight-only inverter), else against the whole day as a fallback.
    v_coverage := CASE
        WHEN v_count = 0 THEN 0
        WHEN v_daylight IS NOT NULL AND v_daylight > 0
            THEN v_span::DOUBLE PRECISION / v_daylight
        ELSE v_span::DOUBLE PRECISION / full_secs
    END;

    -- Continuity: count the distinct 30-second buckets the day's samples fall
    -- into (the same windows power_30sec forms) and compare with the number
    -- expected across the daylight window. Computed from samples directly, so it
    -- needs no continuous aggregate and works for any day still inside sample
    -- retention. Not capped, like coverage: a clear day slightly over 1 is the
    -- horizon signal, not an error.
    SELECT count(DISTINCT time_bucket('30 seconds', time)) INTO v_observed
      FROM samples
     WHERE time >= day_start AND time < day_end;

    v_continuity := CASE
        WHEN v_count = 0 THEN 0
        WHEN v_daylight IS NOT NULL AND v_daylight > 0
            THEN v_observed * 30.0 / v_daylight
        ELSE v_observed * 30.0 / full_secs
    END;

    v_produced := CASE
        WHEN v_count = 0 OR v_first IS NULL OR v_last IS NULL OR v_last < v_first
        THEN NULL ELSE v_last - v_first
    END;

    -- A whole-day outage shows up as zero production; a daylight-edge outage as a
    -- coverage shortfall. The coverage flag only applies when daylight is known
    -- (otherwise coverage is whole-day and not a quality signal). The note
    -- carries the figures for an at-a-glance review; coverage is not capped, so a
    -- clear day slightly over 1 is the signal to lower the site horizon.
    v_notes := CASE
        WHEN v_count = 0        THEN 'no samples for day'
        WHEN v_produced IS NULL THEN 'counter unavailable or went backwards'
        WHEN v_produced = 0     THEN 'needs review: zero production'
        WHEN v_daylight IS NOT NULL AND v_daylight > 0
             AND v_coverage < v_coverage_min
             THEN format('needs review: coverage %s%% (%s/%s s)',
                         round(100.0 * v_coverage), v_span, v_daylight)
        ELSE NULL
    END;

    -- complete is the positive form of the review flag, from the same inputs:
    -- a usable non-zero day whose coverage clears v_coverage_min (or whose
    -- daylight is unknown, so there is no coverage reason to doubt it). Drives
    -- the site rollup.
    v_complete := v_produced IS NOT NULL AND v_produced > 0
                  AND (v_daylight IS NULL OR v_daylight = 0
                       OR v_coverage >= v_coverage_min);

    -- continuous mirrors complete but on continuity: the buckets densely cover
    -- the daylight window. When daylight is unknown we cannot judge gaps, so we
    -- do not doubt it. complete and continuous diverge exactly on a mid-day gap.
    v_continuous := v_count > 0
                    AND (v_daylight IS NULL OR v_daylight = 0
                         OR v_continuity >= v_continuity_min);

    INSERT INTO daily AS d
        (day, produced_kwh, coverage, complete, continuity, continuous,
         sample_count, first_ts, last_ts, first_kwh, last_kwh, computed_at)
    VALUES
        (target_day, v_produced, v_coverage, v_complete, v_continuity, v_continuous,
         v_count, v_first_t, v_last_t, v_first, v_last, now())
    ON CONFLICT (day) DO UPDATE
        SET produced_kwh     = EXCLUDED.produced_kwh,
            coverage         = EXCLUDED.coverage,
            complete         = EXCLUDED.complete,
            continuity       = EXCLUDED.continuity,
            continuous       = EXCLUDED.continuous,
            sample_count     = EXCLUDED.sample_count,
            first_ts         = EXCLUDED.first_ts,
            last_ts          = EXCLUDED.last_ts,
            first_kwh        = EXCLUDED.first_kwh,
            last_kwh         = EXCLUDED.last_kwh,
            computed_at      = EXCLUDED.computed_at;

    out_day          := target_day;
    out_produced_kwh := v_produced;
    out_notes        := v_notes;
    RETURN NEXT;
END;
$$;
