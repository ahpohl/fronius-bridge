-- =============================================================================
-- fronius-bridge: shared public schema (migration 003)
--
-- compute_site_energy(target_day, tz): roll the per-device figures up into one
-- whole-site row in site_energy. Reads the device registry to discover each
-- device's role, so it needs no hardcoded device names and adapts as devices
-- are added or removed. The primary meter's location selects the regime:
--
--   feed-in primary      Regime A (measured): the grid meter sits at the
--                        feed-in point and measures grid exchange directly.
--                        production = sum of the inverters' produced_kwh;
--                        from_grid/to_grid = the grid meter's import/export
--                        counters; self_consumption = production - to_grid;
--                        consumption = self_consumption + from_grid. Exact.
--   consumption primary  Regime B (simulated): grid exchange is not measured.
--                        production and consumption totals still come from the
--                        gap-immune counters -- the inverters' produced_kwh and
--                        the primary meter's import counter, the same kind of
--                        figures Regime A uses -- so the totals are exact in
--                        both regimes. Only self_consumption is estimated, from
--                        the time-aligned overlap of PV power and house power
--                        over 30-second buckets:
--                          self_consumption = sum(min(Pb, Cb)) scaled to kWh
--                        with Pb the inverter power summed across all inverters
--                        and Cb the primary meter's power (both >= 0). It is
--                        clamped to LEAST(estimate, production, consumption) and
--                        the grid figures follow by subtraction:
--                          to_grid   = production  - self_consumption
--                          from_grid = consumption - self_consumption
--                        so both energy balances are exact and non-negative by
--                        construction. The overlap of bucket averages slightly
--                        over-estimates self-consumption (min is concave) and a
--                        mid-day gap biases it low; the contributing devices'
--                        continuous flags gate that (see completeness).
--
-- No sign convention is needed: Regime A reads direction-resolved counters and
-- Regime B overlaps two non-negative magnitudes, deriving grid exchange.
--
-- Ratios (self-usage, self-sufficiency) are not computed here: compute_site_energy
-- emits energy quantities only. Derive them in the dashboard from the kWh
-- columns (self_consumption_kwh / production_kwh and / consumption_kwh).
--
-- complete and continuous are the day's two quality verdicts, each aggregated
-- from the per-device rollups rather than re-derived here, and stored
-- separately so neither is lost. complete is TRUE only when every contributing
-- device-day (all inverters plus the primary meter) is present and itself
-- complete; continuous is TRUE only when every one is itself continuous. They
-- are independent, exactly as on the daily tables: a measured-regime consumer
-- trusts complete alone (its counters are gap-immune, so continuous is only
-- informational there), while the simulated regime is trustworthy on complete
-- AND continuous, since its self-consumption split is summed from power buckets
-- and a mid-day gap -- which complete, being span-based, cannot see -- would
-- bias it. The estimator never sets a device complete, so a backfilled
-- device-day keeps the site incomplete. out_notes lists the device-days that
-- held complete back; it is returned for inspection and not stored.
--
-- Run after the per-device rollups for the same day, since it reads each
-- <device>.daily (and, in the simulated regime, <device>.power_30sec). The
-- convenience wrapper compute_site_rollup() below runs the per-device rollups
-- and then this one in the right order; the nightly cron calls that wrapper.
--
-- ASCII only: this file is folded into the binary via #embed.
-- =============================================================================

CREATE OR REPLACE FUNCTION compute_site_energy(
    target_day DATE,
    tz         TEXT DEFAULT current_setting('TimeZone'))
RETURNS TABLE (out_day                 DATE,
              out_notes                TEXT)
LANGUAGE plpgsql
SET search_path FROM CURRENT
AS $$
DECLARE
    v_primary_loc    TEXT;
    v_primary_schema TEXT;
    v_dev            TEXT;
    v_val            DOUBLE PRECISION;
    v_prod           DOUBLE PRECISION := 0;
    v_cons           DOUBLE PRECISION;
    v_self_cons      DOUBLE PRECISION;
    v_from_grid      DOUBLE PRECISION;
    v_to_grid        DOUBLE PRECISION;
    v_complete       BOOLEAN := TRUE;
    v_continuous     BOOLEAN := TRUE;
    v_dev_complete   BOOLEAN;
    v_dev_continuous BOOLEAN;
    v_incomplete     TEXT;
    v_notes          TEXT;
    -- simulated-regime locals
    v_day_start      TIMESTAMPTZ;
    v_day_end        TIMESTAMPTZ;
    v_prod_union     TEXT;
    v_n_buckets      BIGINT := 0;
    v_sc_pw          DOUBLE PRECISION;
BEGIN
    SELECT r.location, r.device_name
      INTO v_primary_loc, v_primary_schema
      FROM public.device_registry r
     WHERE r.kind = 'meter' AND r.is_primary
     LIMIT 1;

    IF v_primary_loc IS NULL THEN
        out_day   := target_day;
        out_notes := 'no primary meter';
        RETURN NEXT;
        RETURN;
    END IF;

    -- Production: sum produced_kwh across every inverter schema, in both regimes
    -- (gap-immune counter deltas). A missing row contributes nothing; the
    -- completeness pass below notes it.
    FOR v_dev IN SELECT device_name FROM public.device_registry WHERE kind = 'inverter'
    LOOP
        EXECUTE format('SELECT produced_kwh FROM %I.daily WHERE day = $1', v_dev)
          INTO v_val USING target_day;
        IF v_val IS NOT NULL THEN
            v_prod := v_prod + v_val;
        END IF;
    END LOOP;

    -- =======================================================================
    -- Regime A (measured): primary meter at the feed-in point.
    -- =======================================================================
    IF v_primary_loc = 'feed-in' THEN
        -- Grid exchange straight from the primary meter's direction-resolved
        -- import/export counters.
        EXECUTE format('SELECT imported_kwh, exported_kwh FROM %I.daily WHERE day = $1',
                       v_primary_schema)
          INTO v_from_grid, v_to_grid USING target_day;

        IF v_from_grid IS NULL OR v_to_grid IS NULL THEN
            out_day   := target_day;
            out_notes := 'grid meter row missing';
            RETURN NEXT;
            RETURN;
        END IF;

        -- self-consumption is production not exported, floored at 0 (the
        -- inverter and grid meter are separate instruments; noise can put
        -- export marginally above production). consumption = self + imported.
        v_self_cons := GREATEST(v_prod - v_to_grid, 0);
        v_cons      := v_self_cons + v_from_grid;

    -- =======================================================================
    -- Regime B (simulated): primary meter in the consumption path.
    -- =======================================================================
    ELSE
        v_day_start := (target_day::TIMESTAMP AT TIME ZONE tz);
        v_day_end   := ((target_day + 1)::TIMESTAMP AT TIME ZONE tz);

        -- consumption total from the primary meter's import counter (gap-immune,
        -- the same kind of figure Regime A's totals use).
        EXECUTE format('SELECT imported_kwh FROM %I.daily WHERE day = $1',
                       v_primary_schema)
          INTO v_cons USING target_day;

        IF v_cons IS NULL THEN
            out_day   := target_day;
            out_notes := 'consumption meter row missing';
            RETURN NEXT;
            RETURN;
        END IF;

        -- self-consumption is the only estimated quantity: the time-aligned
        -- overlap of PV power and house power from 30-second buckets. Production
        -- power is summed per bucket across all inverters.
        v_prod_union := '';
        FOR v_dev IN SELECT device_name FROM public.device_registry WHERE kind = 'inverter'
        LOOP
            IF v_prod_union <> '' THEN
                v_prod_union := v_prod_union || ' UNION ALL ';
            END IF;
            v_prod_union := v_prod_union || format(
                'SELECT bucket, GREATEST(avg_power, 0) AS p '
                'FROM %I.power_30sec WHERE bucket >= $1 AND bucket < $2', v_dev);
        END LOOP;

        IF v_prod_union = '' THEN
            out_day   := target_day;
            out_notes := 'no inverters configured';
            RETURN NEXT;
            RETURN;
        END IF;

        EXECUTE format(
            'WITH prod AS (SELECT bucket, sum(p) AS pw FROM (%s) u GROUP BY bucket), '
            'cons AS (SELECT bucket, GREATEST(avg_power, 0) AS cw FROM %I.power_30sec '
                     'WHERE bucket >= $1 AND bucket < $2), '
            'j AS (SELECT COALESCE(prod.pw, 0) AS pw, COALESCE(cons.cw, 0) AS cw '
                  'FROM prod FULL OUTER JOIN cons USING (bucket)) '
            'SELECT count(*), sum(least(pw, cw)) FROM j',
            v_prod_union, v_primary_schema)
          INTO v_n_buckets, v_sc_pw
          USING v_day_start, v_day_end;

        IF v_n_buckets = 0 THEN
            out_day   := target_day;
            out_notes := 'no power data for day';
            RETURN NEXT;
            RETURN;
        END IF;

        -- Scale the overlap to kWh (W * 30s -> kWh) and clamp to the counter
        -- totals. sum(min) cannot exceed either total in theory, but the bucket
        -- estimate and the counters are independent measurements, so LEAST keeps
        -- both derived grid figures non-negative and the balances exact.
        v_self_cons := LEAST(COALESCE(v_sc_pw, 0) * 30.0 / 3600.0 / 1000.0,
                             v_prod, v_cons);
        v_to_grid   := v_prod - v_self_cons;
        v_from_grid := v_cons - v_self_cons;
    END IF;

    -- Completeness and continuity: aggregate the two per-device verdicts
    -- independently, exactly as the daily rollups carry them -- complete is the
    -- AND of every contributing device's complete (its samples spanned the day),
    -- continuous the AND of every continuous (its 30-second buckets densely cover
    -- the window). Both are stored, so neither verdict is lost or conflated: the
    -- measured regime is trusted on complete alone (gap-immune counters), the
    -- simulated regime on complete AND continuous (its split is bucket-summed, so
    -- a mid-day gap that complete cannot see would bias it). A missing daily row
    -- fails both, just like a flagged one. out_notes lists the device-days that
    -- held complete back.
    FOR v_dev IN
        SELECT device_name FROM public.device_registry WHERE kind = 'inverter'
        UNION ALL
        SELECT v_primary_schema
    LOOP
        EXECUTE format('SELECT complete, continuous FROM %I.daily WHERE day = $1', v_dev)
          INTO v_dev_complete, v_dev_continuous USING target_day;
        IF v_dev_complete IS NOT TRUE THEN
            v_complete   := FALSE;
            v_incomplete := CASE WHEN v_incomplete IS NULL THEN v_dev
                                 ELSE v_incomplete || ', ' || v_dev END;
        END IF;
        IF v_dev_continuous IS NOT TRUE THEN
            v_continuous := FALSE;
        END IF;
    END LOOP;

    IF v_incomplete IS NOT NULL THEN
        v_notes := 'incomplete: ' || v_incomplete;
    END IF;

    -- Upsert. from_grid/to_grid hold the day's grid exchange (measured counters
    -- in Regime A; derived from the counter totals and the estimated split in
    -- Regime B).
    INSERT INTO public.site_energy AS d
        (day, production_kwh, consumption_kwh, self_consumption_kwh,
         from_grid_kwh, to_grid_kwh, complete, continuous, computed_at)
    VALUES
        (target_day, v_prod, v_cons, v_self_cons,
         v_from_grid, v_to_grid, v_complete, v_continuous, now())
    ON CONFLICT (day) DO UPDATE
        SET production_kwh       = EXCLUDED.production_kwh,
            consumption_kwh      = EXCLUDED.consumption_kwh,
            self_consumption_kwh = EXCLUDED.self_consumption_kwh,
            from_grid_kwh        = EXCLUDED.from_grid_kwh,
            to_grid_kwh          = EXCLUDED.to_grid_kwh,
            complete             = EXCLUDED.complete,
            continuous           = EXCLUDED.continuous,
            computed_at          = EXCLUDED.computed_at;

    out_day   := target_day;
    out_notes := v_notes;
    RETURN NEXT;
END;
$$;

-- -----------------------------------------------------------------------------
-- compute_site_rollup(target_day, tz)
--
-- Convenience orchestrator for one day's rollups: run every device's per-kind
-- rollup and then the whole-site rollup, in the order compute_site_energy needs
-- (it reads the <device>.daily rows the per-device functions write). Device
-- schemas are discovered by the rollup function they expose --
-- compute_inverter_energy for inverters, compute_meter_energy for meters -- so
-- this needs no device names and adapts as hardware is added or removed.
--
-- This is what the nightly cron calls (see DEPLOYMENT.md): a single statement
-- for the whole site instead of looping the per-device functions by hand. Run it
-- by hand to recompute a day immediately. target_day is read as a wall-clock day
-- in tz (default the session TimeZone), the same convention the called functions
-- use, so a DST day stays correct as long as the same tz is passed through.
--
-- pg_catalog is searched implicitly, and every call is schema-qualified, so no
-- search_path binding is needed here.
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION compute_site_rollup(
    target_day DATE,
    tz         TEXT DEFAULT current_setting('TimeZone'))
RETURNS VOID
LANGUAGE plpgsql
AS $$
DECLARE
    rec RECORD;
BEGIN
    -- Per-device rollups first. Each device schema exposes exactly one of the
    -- two per-kind functions; call whichever it has.
    FOR rec IN
        SELECT n.nspname AS schema, p.proname AS fn
          FROM pg_proc p
          JOIN pg_namespace n ON n.oid = p.pronamespace
         WHERE p.proname IN ('compute_inverter_energy', 'compute_meter_energy')
    LOOP
        EXECUTE format('SELECT %I.%I($1, $2)', rec.schema, rec.fn)
          USING target_day, tz;
    END LOOP;

    -- Then the whole-site rollup for the same day, after every <device>.daily
    -- row it reads has been written above.
    PERFORM public.compute_site_energy(target_day, tz);
END;
$$;
