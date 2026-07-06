-- =============================================================================
-- fronius-bridge: per-device inverter schema (migration 005)
--
-- Measure coverage over the production span instead of the whole day.
--
-- The 002 version measured the first..last span and the 30-second buckets
-- over the whole calendar day, assuming samples only exist while the sun is
-- up. With inverter standby disabled the SunSpec interface answers around the
-- clock, a day collects ~24h of honest 0 W samples, and both metrics blow
-- past 1 against the ~16h daylight denominator.
--
-- This replacement bounds the span by production itself: the first and the
-- last power_30sec bucket whose every sample produced (the aggregate already
-- carries per-bucket min_power). 0 W samples cannot move the bounds, so
-- standby-off nights and the inverter's awake-but-idle buffer around
-- dawn/dusk are clipped by construction. min_power (rather than max_power)
-- makes a bound immune to an isolated spurious reading -- its 0 W bucket
-- neighbours keep min_power at 0 -- at the cost of clipping sub-bucket
-- flicker around cut-in, which is not steady production anyway. Moving a
-- bound would take a full bucket of consecutive reads surviving Modbus RTU's
-- CRC-16.
--
-- solar_daylight() is reduced to the denominator; nothing filters samples by
-- the computed window, so coverage keeps its two-sided calibration property:
-- over 1 means the computed daylight is too short (lower
-- public.site.horizon_deg), a persistent clear-day shortfall means it is too
-- long (raise it, per its comment in 004).
--
-- The energy figures are untouched: the counter is frozen at night, so the
-- daily delta does not care about the span, and the whole-day boundary
-- readings are what the cross-midnight gap-estimator wants.
--
-- ASCII only: this file is folded into the binary via #embed into a char
-- array, so any non-ASCII byte would break the build.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- The observed production bounds, stored. daily outlives every other record
-- of them (samples drop at 90 days, power_30sec at 2 years), coverage and
-- continuity become reproducible from the row alone, and plotting them
-- against solar_daylight()'s sunrise/sunset is the horizon calibration
-- picture. NULL means no production was observed that day. Rows computed
-- before this migration stay NULL until recomputed.
-- -----------------------------------------------------------------------------
ALTER TABLE daily
    ADD COLUMN production_start TIMESTAMPTZ,
    ADD COLUMN production_end   TIMESTAMPTZ;

-- -----------------------------------------------------------------------------
-- compute_inverter_energy(target_day, tz)
--
-- Computes this inverter's daily production as the delta of the ac_energy
-- counter across the day and upserts one row into daily. The day's first/last
-- counter readings and their timestamps are stored alongside the delta for
-- the cross-midnight gap-estimator.
--
-- The quality metrics judge the production span (see the header): coverage is
-- the span as a fraction of the expected daylight from solar_daylight()
-- (public, fed lat/long and the horizon angle from public.site), continuity
-- the share of the expected 30-second buckets present between the bounds.
-- Both are uncapped: slightly over 1 is the signal to lower the site horizon.
-- coverage below v_coverage_min is flagged 'needs review', as is zero
-- production. These are data-completeness metrics, not production-health
-- metrics: a mid-day drop to 0 W reads like an overcast spell and shows in
-- produced_kwh instead.
--
-- complete and continuous are the positive forms of the two flags; they
-- diverge on a mid-day collection gap, which continuity sees but span-based
-- coverage cannot. continuous gates the simulated site rollup, whose
-- self-consumption split is summed from the same power_30sec buckets. When
-- the site has no coordinates, daylight is unknown: the denominator falls
-- back to the whole day (DST-correct) and only the zero-production flag
-- applies.
--
-- Returns the day, the produced kWh and a notes string (NULL when clean).
--
-- SET search_path FROM CURRENT binds the function to this device's schema at
-- creation time, so samples/daily/power_30sec resolve here and the
-- timescaledb helpers in public. tz defaults to the session TimeZone;
-- target_day is read as a wall-clock day in that zone, keeping 23h/25h DST
-- days correct.
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
    -- Review thresholds: the span must cover this fraction of the expected
    -- daylight (production missing at dawn/dusk), and the 30-second buckets
    -- must too (a mid-day collection outage span-based coverage cannot see).
    v_coverage_min   CONSTANT NUMERIC := 0.90;
    v_continuity_min CONSTANT NUMERIC := 0.90;
    v_count    INTEGER;
    v_first_t  TIMESTAMPTZ;
    v_last_t   TIMESTAMPTZ;
    v_first    DOUBLE PRECISION;   -- ac_energy counter at first sample
    v_last     DOUBLE PRECISION;   -- ac_energy counter at last sample
    v_lat      DOUBLE PRECISION;   -- site coords, for the daylight denominator
    v_lon      DOUBLE PRECISION;
    v_horizon  DOUBLE PRECISION;   -- site horizon angle for solar_daylight
    v_daylight INTEGER;            -- expected daylight seconds; NULL if no coords
    v_prod_lo  TIMESTAMPTZ;        -- production bounds: first/last power_30sec
    v_prod_hi  TIMESTAMPTZ;        -- bucket with non-zero power (see header);
                                   -- stored as production_start/production_end
    v_span_lo  TIMESTAMPTZ;        -- metric bounds: the production bounds, or
    v_span_hi  TIMESTAMPTZ;        -- first/last sample when there are none
    v_span     INTEGER;            -- production span in seconds
    v_coverage DOUBLE PRECISION;   -- span / daylight (or / day when no coords)
    v_complete BOOLEAN;            -- day fully measured (positive form)
    v_observed   INTEGER;          -- power_30sec buckets between the bounds
    v_continuity DOUBLE PRECISION; -- observed / expected buckets (gap-aware)
    v_continuous BOOLEAN;          -- buckets densely cover the daylight
    v_produced DOUBLE PRECISION;   -- daily production (last - first counter)
    v_notes    TEXT;
BEGIN
    -- Whole-day pass: the counter delta and the boundary readings. These
    -- deliberately ignore the production span -- the counter is frozen at
    -- night, and the gap-estimator wants the outermost observed boundaries.
    SELECT count(*), min(time), max(time),
           first(ac_energy, time), last(ac_energy, time)
      INTO v_count, v_first_t, v_last_t, v_first, v_last
      FROM samples
     WHERE time >= day_start AND time < day_end
       AND ac_energy IS NOT NULL;

    -- Retention guard: raw samples drop at 90 days, power_30sec lives 2
    -- years. Recomputing a day whose samples have aged out would overwrite a
    -- good row with zeros, so preserve the existing row and only report. A
    -- genuinely empty new day still gets its zero row below.
    IF v_count = 0 AND EXISTS (SELECT 1 FROM daily WHERE day = target_day) THEN
        out_day          := target_day;
        out_produced_kwh := NULL;
        out_notes        := 'no samples for day; existing row preserved';
        RETURN NEXT;
        RETURN;
    END IF;

    -- Expected daylight for the day. NULL (no coords) or 0 (polar night)
    -- means we cannot judge against daylight.
    SELECT latitude, longitude, horizon_deg INTO v_lat, v_lon, v_horizon
      FROM public.site;
    IF v_lat IS NOT NULL AND v_lon IS NOT NULL THEN
        SELECT seconds INTO v_daylight
          FROM public.solar_daylight(v_lat, v_lon, target_day, tz, v_horizon);
    END IF;

    -- Production bounds: start of the first to end of the last bucket whose
    -- every sample produced (min_power, see the header). The raw bounds are
    -- stored (NULL when the day never produced); the metrics fall back to the
    -- day's first/last sample when there are none, or when the aggregate is
    -- not yet materialised -- after a sample backfill, refresh power_30sec
    -- (see 003) before recomputing days.
    SELECT min(bucket), max(bucket) + interval '30 seconds'
      INTO v_prod_lo, v_prod_hi
      FROM power_30sec
     WHERE bucket >= day_start AND bucket < day_end
       AND min_power > 0;
    v_span_lo := COALESCE(v_prod_lo, v_first_t);
    v_span_hi := COALESCE(v_prod_hi, v_last_t);

    v_span := CASE WHEN v_count > 0
                   THEN EXTRACT(EPOCH FROM (v_span_hi - v_span_lo))::INTEGER
                   ELSE 0 END;

    v_coverage := CASE
        WHEN v_count = 0 THEN 0
        WHEN v_daylight IS NOT NULL AND v_daylight > 0
            THEN v_span::DOUBLE PRECISION / v_daylight
        ELSE v_span::DOUBLE PRECISION / full_secs
    END;

    -- A power_30sec bucket exists exactly where samples do (NULL-power ones
    -- included), so counting them between the bounds measures collection.
    -- In steady state the aggregate lags the raw data by at most ~90s,
    -- shaving at most a bucket or two off a same-day computation; the row is
    -- recomputed the next day anyway.
    SELECT count(*) INTO v_observed
      FROM power_30sec
     WHERE bucket >= v_span_lo AND bucket < v_span_hi;

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

    -- The coverage flag only applies when daylight is known (otherwise
    -- coverage is whole-day and not a quality signal). The note carries the
    -- figures for an at-a-glance review.
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

    -- The positive forms of the two review flags, from the same inputs. With
    -- daylight unknown there is no reason to doubt the day, so neither flag
    -- fires on the fallback denominator.
    v_complete := v_produced IS NOT NULL AND v_produced > 0
                  AND (v_daylight IS NULL OR v_daylight = 0
                       OR v_coverage >= v_coverage_min);
    v_continuous := v_count > 0
                    AND (v_daylight IS NULL OR v_daylight = 0
                         OR v_continuity >= v_continuity_min);

    INSERT INTO daily AS d
        (day, produced_kwh, coverage, complete, continuity, continuous,
         sample_count, first_ts, last_ts, first_kwh, last_kwh,
         production_start, production_end, computed_at)
    VALUES
        (target_day, v_produced, v_coverage, v_complete, v_continuity, v_continuous,
         v_count, v_first_t, v_last_t, v_first, v_last,
         v_prod_lo, v_prod_hi, now())
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
            production_start = EXCLUDED.production_start,
            production_end   = EXCLUDED.production_end,
            computed_at      = EXCLUDED.computed_at;

    out_day          := target_day;
    out_produced_kwh := v_produced;
    out_notes        := v_notes;
    RETURN NEXT;
END;
$$;
