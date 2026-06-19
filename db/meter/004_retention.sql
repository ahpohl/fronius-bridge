-- =============================================================================
-- fronius-bridge: per-device meter schema (migration 004)
--
-- Storage lifecycle: columnstore compression and data retention, kept together
-- so the whole storage budget is visible at a glance. The strategy is tiered --
-- raw samples are bulky and only needed transiently, the downsampled aggregates
-- are the cheap long-term record:
--
--   samples / phase_samples   compress 7d, drop 90 days
--   power_5min                compress 7d, drop 2 years
--   power_30sec               compress 7d, drop 2 years
--
-- Raw samples can age out at 90 days because the permanent record lives
-- elsewhere: the daily rollup (and public.site_energy) are tiny plain tables,
-- one row per day, never dropped, and the daily *_kwh_first/_kwh_last columns
-- preserve the boundary counter readings the gap-estimator needs. Both
-- aggregates are kept 2 years: power_5min backs long-range plots, and power_30sec
-- is the high-fidelity source the simulated self-consumption split is summed
-- from, so it must outlive the raw samples to keep that recompute available.
--
-- Compression (hypercore columnstore) converts chunks older than 7 days to a
-- compressed columnar form -- still queried transparently, an order of magnitude
-- or more smaller. The bridge only ever writes recent (uncompressed) chunks and
-- the rollups only read, so nothing rewrites a compressed chunk; recompute over
-- old data just decompresses on read. Energy counters are monotonic and compress
-- especially well (delta-of-delta). segmentby groups the per-phase series of
-- phase_samples; the single-series samples and the aggregates take none.
--
-- Dropping raw data while keeping the aggregates is safe because each aggregate's
-- refresh window (migration 003: at most 2 hours back) never reaches the 90-day
-- boundary, so a refresh never re-derives -- and so never deletes -- aggregated
-- rows over dropped raw chunks.
--
-- enable_columnstore is a table option (idempotent). add_columnstore_policy and
-- add_retention_policy only register background jobs -- they move no data inline
-- -- so this migration stays transaction-safe. if_not_exists keeps re-application
-- harmless.
--
-- ASCII only: this file is folded into the binary via #embed.
-- =============================================================================

-- Columnstore compression: convert chunks to the compressed columnar store once
-- they age past the 7-day active-write window.
ALTER TABLE samples SET (
    timescaledb.enable_columnstore = TRUE,
    timescaledb.orderby            = 'time DESC');
ALTER TABLE phase_samples SET (
    timescaledb.enable_columnstore = TRUE,
    timescaledb.segmentby          = 'phase_id',
    timescaledb.orderby            = 'time DESC');
ALTER MATERIALIZED VIEW power_5min  SET (timescaledb.enable_columnstore = TRUE);
ALTER MATERIALIZED VIEW power_30sec SET (timescaledb.enable_columnstore = TRUE);

CALL add_columnstore_policy('samples',
    after => INTERVAL '7 days', if_not_exists => TRUE);
CALL add_columnstore_policy('phase_samples',
    after => INTERVAL '7 days', if_not_exists => TRUE);
CALL add_columnstore_policy('power_5min',
    after => INTERVAL '7 days', if_not_exists => TRUE);
CALL add_columnstore_policy('power_30sec',
    after => INTERVAL '7 days', if_not_exists => TRUE);

-- Retention: drop raw chunks at 90 days; keep both aggregates for 2 years.
SELECT add_retention_policy('samples',
    drop_after => INTERVAL '90 days', if_not_exists => TRUE);
SELECT add_retention_policy('phase_samples',
    drop_after => INTERVAL '90 days', if_not_exists => TRUE);
SELECT add_retention_policy('power_5min',
    drop_after => INTERVAL '2 years', if_not_exists => TRUE);
SELECT add_retention_policy('power_30sec',
    drop_after => INTERVAL '2 years', if_not_exists => TRUE);
