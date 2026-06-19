# Deploying the PostgreSQL consumer

This guide covers the optional PostgreSQL time-series consumer: database
prerequisites, the one-time privileged setup, how the bridge lays out data, and
the operational workflows. The bridge runs MQTT-only when no `postgres` section
is configured; none of this is required in that case.

## Data model: one schema per device

Each configured device gets its own PostgreSQL schema named after the device
(`primo`, `grid`, `heatpump`, ...); the schema name *is* the device identity,
with no numeric device id. A small shared `public.device_registry` records each
device's role and location for the site rollup (`compute_site_energy()` reads
it), but the schema remains the key. A schema is self-contained:
its `samples`/`phase_samples`/`input_samples` hypertables, a `device` row, a
wide `daily` rollup table, a per-kind rollup function
(`compute_inverter_energy()` or `compute_meter_energy()`), and a
`schema_version` ledger. Whole-site figures — total production, grid exchange,
self-consumption — are derived into `public.site_energy` by
`compute_site_energy()` (see [Site energy](#site-energy)).

On a device's first reading the bridge creates that device's schema and objects
(or, under `--no-migrate`, verifies they already exist). Each `daily` row covers
one day: an inverter row carries `produced_kwh`; a meter row carries
`imported_kwh` and `exported_kwh`. Every row also records how much of the day the samples
covered (`coverage` and `continuity`, fractions in `[0, 1]`, plus `sample_count`), two
quality flags (`complete` and `continuous`), and the day's boundary counter readings.

## Requirements

- **PostgreSQL** 14 or newer.
- **TimescaleDB** — the per-device sample tables are hypertables, and the daily
  rollup uses the `first()`/`last()` aggregates.
- **pg_cron** — drives the nightly rollup job (see [Daily rollups](#daily-rollups-with-pg_cron)).

Installing extensions is a privileged operation; the bridge never attempts it.
At startup the worker verifies only that `timescaledb` is present **in its own
database** and exits with a fatal error if it is missing. It does **not** check
for `pg_cron`: that extension is a cluster-level concern that frequently lives
in a different database, and the bridge never calls it directly.

## Cluster configuration (postgresql.conf)

Both extensions are loaded at server start, so they belong in
`shared_preload_libraries`, and `pg_cron` must be told which database holds its
bookkeeping. These are cluster-wide settings in `postgresql.conf` and a restart
is required for them to take effect:

```conf
# Both libraries must be preloaded. timescaledb is always required; pg_cron is
# only needed if you run the nightly rollup (see "Daily rollups" below).
shared_preload_libraries = 'timescaledb,pg_cron'

# The single database whose pg_cron extension is created and where the cron.job
# tables live. It need not be the bridge's database -- 'postgres' (the default)
# or a dedicated 'pg_cron' database is fine. Jobs scheduled here still target
# the bridge's database via cron.schedule_in_database(...).
cron.database_name = 'postgres'

# Timezone the cron *schedules* are interpreted in, i.e. when a job fires. Set
# it to your local zone so e.g. '15 0 * * *' means 00:15 local. This does NOT
# set the rollup's day boundary; that is passed explicitly (see "Daily
# rollups").
cron.timezone = 'Europe/Berlin'
```

After restarting the server, create the `pg_cron` extension once, in the
database named by `cron.database_name`:

```sql
-- Connected to the cron.database_name database (e.g. postgres):
CREATE EXTENSION IF NOT EXISTS pg_cron;
```

`timescaledb` is created separately, in the bridge's own database, in the next
step. If you are not running the rollup you can drop `pg_cron` from
`shared_preload_libraries` and skip its extension entirely; the bridge never
requires it and verifies only that `timescaledb` is present in its own database.

## One-time database setup

Run as a PostgreSQL superuser. Replace the database name, role name, and
password to taste.

```sql
-- 1. Database
CREATE DATABASE fronius;
\connect fronius

-- 2. TimescaleDB, in the bridge's database (privileged; the bridge checks for
--    it but never installs it). pg_cron is NOT created here -- it lives in its
--    own database on this cluster; see "Daily rollups" below.
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- 3. Role the bridge connects as
--    create a secure and postgresql compatible password with
--    tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 32; echo
CREATE ROLE fronius_bridge LOGIN PASSWORD 'change-me';

-- 4. Let the bridge create its per-device schemas in this database
GRANT CREATE ON DATABASE fronius TO fronius_bridge;
```

`GRANT CREATE ON DATABASE` is what lets the bridge run `CREATE SCHEMA` for each
device. The role owns every schema it creates, so it needs no further grants to
write its own tables. The `timescaledb` functions live in `public`; the default
`PUBLIC` `USAGE`/`EXECUTE` grants cover them. If you have revoked the default
`PUBLIC` grants, also `GRANT USAGE ON SCHEMA public TO fronius_bridge`.

### Least-privilege alternative

If you would rather not grant `CREATE ON DATABASE`, create the schemas yourself
(apply the per-kind SQL in `db/inverter/` and `db/meter/`, installed under
`<datadir>/fronius-bridge/db/`, into a schema named after each device), grant
the role `USAGE` on each schema and `INSERT` on its tables, and run the bridge
with `--no-migrate`. In that mode the bridge only verifies each schema is at the
expected version and writes rows; it creates nothing.

## Connecting the bridge

Add a `postgres` section to the YAML config (see the README's configuration
reference for every key). The DSN is a standard libpq connection string:

```yaml
postgres:
  dsn: "host=localhost port=5432 dbname=fronius user=fronius_bridge password=change-me"
  queue_size: 10000
  reconnect_delay:
    min: 5
    max: 60
    exponential: true
```

The worker connects lazily and reconnects with backoff, so a database that is
unreachable at startup does not block the rest of the bridge. A configuration,
schema, privilege, or missing-extension error is fatal and shuts the bridge
down with a non-zero exit so the operator notices; transient connection errors
are retried.

## Daily rollups with pg_cron

Schedule two jobs, both calling `compute_site_rollup`: one finalizes the
previous day just after the local-midnight rollover, and one refreshes the
current day every few minutes so the dashboard sees today's running figures.
Connect to the database where `pg_cron` is installed (e.g. your `pg_cron`
database) and use `cron.schedule_in_database(...)` with the bridge's database as
the target — `pg_cron` does not need to live in the bridge's database:

```sql
-- Run this while connected to the database where pg_cron is installed.

-- Finalize the previous day, just after the local-midnight rollover.
SELECT cron.schedule_in_database(
  'fronius-daily-rollup',
  '3 0 * * *',                           -- 00:03 every day
  $job$
    SELECT public.compute_site_rollup(
      (now() AT TIME ZONE current_setting('cron.timezone'))::date - 1,
      current_setting('cron.timezone'));
  $job$,
  'fronius'                              -- target: the bridge database
);

-- Refresh the current day so today's running totals stay fresh.
SELECT cron.schedule_in_database(
  'fronius-current-day-rollup',
  '*/5 * * * *',                         -- every 5 minutes
  $job$
    SELECT public.compute_site_rollup(
      (now() AT TIME ZONE current_setting('cron.timezone'))::date,
      current_setting('cron.timezone'));
  $job$,
  'fronius'
);
```

The command runs *in* the target database, so `compute_site_rollup` and the
per-device functions it calls resolve against the bridge's schemas even though
the job is registered from the `pg_cron` database. If your `cron.database_name`
already points at the bridge database, use `cron.schedule(...)` without the
trailing database argument.

`tz` sets the day boundary, and it matters. `compute_site_rollup` reads the day
as a wall-clock day in `tz`, and the jobs compute "yesterday" and "today" in
that same zone — `current_setting('cron.timezone')`, the `cron.timezone` set in
`postgresql.conf` — so the rollup follows your local calendar and stays correct
across 23h/25h DST days. To follow a different zone than the schedule fires in,
pass it literally, e.g. `compute_site_rollup((now() AT TIME ZONE 'Europe/Berlin')::date - 1,
'Europe/Berlin')`.

The finalize runs at 00:03, between the current-day ticks at 00:00 and 00:05, so
it runs on its own and the finished day's last samples have had time to land. The
two jobs only ever write different `day` rows and both upsert, so even an
overlapping run would be harmless — the offset just keeps them from doing
redundant work at the same instant.

Until a day is over, its current-day row is a live, partial view: each refresh
recomputes the day so far, so the totals trail real time by up to the refresh
interval. Because `coverage` and `continuity` are measured against the *whole*
day's window — full daylight for inverters, the 24h day for meters — the quality
flags settle only late: a clean day clears `continuous` once most of the day has
elapsed and `complete` only in its final minutes, and even the last current-day
run, at 23:55, stops short of midnight. `fronius-daily-rollup` recomputes the
finished day end to end and writes its authoritative totals and final flags, so
treat `max(day)` in `site_energy` as live and filter settled history on
`complete` / `continuous` (see [Site energy](README.md#site-energy)).

## Function reference

The consumer installs the SQL functions below — call them by hand to recompute a
day, see why one was flagged, or read the site's daylight length. All ship in the
migrations under `<datadir>/fronius-bridge/db/` and live either in a device schema
or in `public`.

### `<inverter>.compute_inverter_energy` / `<meter>.compute_meter_energy` `(target_day DATE, tz TEXT)`

Computes one device's energy for `target_day` (read as a wall-clock day in `tz`,
default the session `TimeZone`) and upserts a row into
`<schema>.daily`. Each device kind has its own per-kind function — inverters
expose `compute_inverter_energy`, meters `compute_meter_energy`.

```sql
-- inverter schema (out_: day, produced_kwh, notes)
SELECT * FROM primo.compute_inverter_energy('2026-06-05', 'Europe/Berlin');

-- meter schema (out_: day, imported_kwh, exported_kwh, notes)
SELECT * FROM grid.compute_meter_energy('2026-06-05', 'Europe/Berlin');
```

`coverage` is the sampled span over the expected window (the whole day for a
meter, daylight for an inverter); `continuity` is the fraction of expected
30-second buckets actually present — the gap-aware signal `coverage` cannot see.
`complete` / `continuous` are their booleans (`continuity >= 0.90`). Recompute a span
of days by looping:

```sql
SELECT primo.compute_inverter_energy(d::date, 'Europe/Berlin')
FROM generate_series('2026-06-01'::date, '2026-06-07'::date,
                     interval '1 day') AS g(d);
```

### `public.compute_site_energy(target_day DATE, tz TEXT)`

Rolls the per-device `daily` rows for `target_day` into one `public.site_energy`
row. It reads `public.device_registry` for each device's role, so it needs no
device names; run it *after* the per-device rollups for that day.

```sql
SELECT * FROM public.compute_site_energy('2026-06-05', 'Europe/Berlin');
```

The function returns only `day` and `notes`; the verdicts land in `public.site_energy`,
where `complete` and `continuous` are the two stored signals (use `complete` for a
measured day, `complete AND continuous` for a simulated one). The per-regime arithmetic
is in [Grid meter placement](README.md#grid-meter-placement).

### `public.compute_site_rollup(target_day DATE, tz TEXT)`

Convenience wrapper that runs a whole day in one call: every device's per-kind
rollup, then `compute_site_energy`, in that order. It finds the device schemas by
the rollup function they expose, so it needs no device names and follows hardware
as it is added or removed. This is what the nightly cron calls; run it by hand to
recompute a day end-to-end:

```sql
-- for a single day
SELECT public.compute_site_rollup('2026-06-05', 'Europe/Berlin');

-- for all available days (date span taken from one device's samples;
-- use whichever schema reaches back furthest)
SELECT public.compute_site_rollup(d::date, 'Europe/Berlin')
FROM generate_series((SELECT min(time) FROM primo.samples)::date,
                     (SELECT max(time) FROM primo.samples)::date,
                     interval '1 day') AS g(d);
```

It returns `void` — read `public.site_energy` (and the per-device `daily` tables)
afterwards. `tz` defaults to the session `TimeZone`; pass it explicitly (as the cron job
does) when the energy "day" should follow a specific zone.

### `public.solar_daylight(lat, lon, for_day [, tz [, horizon_deg]])`

Sunrise, sunset and daylight length for a point and date — the basis for an
inverter day's daylight-relative `coverage` and `continuity`. Returns `daylight`
(INTERVAL), `seconds` (INTEGER) and `sunrise` / `sunset` (absolute instants).
`tz` defaults to the session `TimeZone`; `horizon_deg` defaults to `-0.833`
(geometric sunrise, refraction included).

```sql
-- daylight length of a given day at the site, from the stored coordinates
SELECT d.daylight, d.seconds, d.sunrise, d.sunset
FROM public.site s,
     LATERAL public.solar_daylight(s.latitude, s.longitude,
                                   '2026-06-21', 'Europe/Berlin', s.horizon_deg) d;

-- or pass coordinates directly, no site row needed
SELECT * FROM public.solar_daylight(49.07, 9.15, '2026-12-21', 'Europe/Berlin');
```

The first form reads the one-row `public.site` table the bridge fills from the
config's `site.latitude` / `site.longitude`; the second is standalone. A polar
day or night returns `seconds` of `86400` or `0` with `sunrise`/`sunset` NULL.

## Site energy

Whole-site totals don't need cross-schema summation. `public.site_energy`
already carries them — one row per day, written by the nightly
`compute_site_energy()` rollup: production summed across every inverter, plus
consumption, self-consumption and grid import/export resolved per the primary
meter's regime (see [Grid meter placement](README.md#grid-meter-placement)). Reading per-meter
`imported_kwh`/`exported_kwh` across schemas would instead double-count
sub-meters, such as a dedicated heat-pump meter. Read it straight out, deriving
the ratios on the fly:

```sql
SELECT day,
       self_consumption_kwh / NULLIF(consumption_kwh, 0) AS self_sufficiency,
       self_consumption_kwh / NULLIF(production_kwh, 0)  AS self_usage,
       complete, continuous
FROM public.site_energy
ORDER BY day;
```

`self_sufficiency` (self-consumption / consumption, "autarky") and `self_usage`
(self-consumption / production) are fractions in `[0, 1]` (multiply by 100 for a
percentage); `NULLIF` yields `NULL` when the denominator is zero — no consumption
or no production that day. `complete` and `continuous` are the day's two quality
verdicts, each aggregated from the contributing device-days: trust a measured
(feed-in) day on `complete` alone, and a simulated (consumption) day on
`complete AND continuous` (see [Site energy](README.md#site-energy)). Filter on
them, or keep flagged days and dim them in the dashboard.

## Data completeness statistics

Count the complete and continuous days:

```sql
SELECT count(*) AS days, min(day) AS first_day, max(day) AS last_day,
       count(*) FILTER (WHERE complete)   AS complete_days,
       count(*) FILTER (WHERE continuous) AS continuous_days
FROM public.site_energy;
```
