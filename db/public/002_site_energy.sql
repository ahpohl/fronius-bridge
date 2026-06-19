-- =============================================================================
-- fronius-bridge: shared public schema (migration 002)
--
-- site_energy: one row per day of whole-site energy figures, derived from the
-- per-device daily rollups by compute_site_energy() (migration 003). The same
-- table serves both regimes -- measured (a bidirectional grid meter at the
-- feed-in point) and simulated (100% feed-in, reconstructed from the power
-- overlap) -- with identical column meanings; which regime a deployment uses is
-- fixed by its topology and documented in the README, so no per-row marker is
-- carried here.
--
-- No ratios are stored: self-usage (self_consumption / production) and
-- self-sufficiency (self_consumption / consumption) are both trivially derived
-- in a dashboard query, so site_energy holds energy quantities (kWh) only, plus
-- the two quality verdicts.
--
-- complete and continuous are the day's quality verdicts, each aggregated from
-- the per-device daily rollups rather than re-checked here, mirroring the two
-- flags those tables carry: complete is TRUE only when every contributing
-- device-day (all inverters plus the primary meter) is present and itself
-- complete (its samples spanned the day); continuous is TRUE only when every one
-- is itself continuous (its 30-second buckets densely cover the expected
-- window). They are stored separately so neither is lost: a consumer trusts the
-- measured regime on complete alone (its counter arithmetic is gap-immune) and
-- the simulated regime on complete AND continuous (its self-consumption split is
-- summed from those buckets, so a mid-day gap -- invisible to complete -- would
-- bias it).
--
-- ASCII only: this file is folded into the binary via #embed.
-- =============================================================================

CREATE TABLE IF NOT EXISTS site_energy (
    day                   DATE PRIMARY KEY,
    production_kwh        DOUBLE PRECISION,   -- PV generated
    consumption_kwh       DOUBLE PRECISION,   -- total site load
    self_consumption_kwh  DOUBLE PRECISION,   -- PV consumed on site (production - to_grid)
    from_grid_kwh         DOUBLE PRECISION,   -- imported from grid
    to_grid_kwh           DOUBLE PRECISION,   -- exported to grid
    complete              BOOLEAN NOT NULL DEFAULT FALSE,  -- all device-days complete
    continuous            BOOLEAN NOT NULL DEFAULT FALSE,  -- all device-days continuous
    computed_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);
