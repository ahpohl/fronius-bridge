-- =============================================================================
-- fronius-bridge: public schema (migration 004)
--
-- Site location support for daylight-relative coverage.
--
-- Latitude/longitude are a property of the installation, not of any one device
-- (every inverter at the site shares them), so they live in a single-row
-- public.site table rather than being duplicated across the device registry.
-- The bridge upserts the row from the YAML `site:` section at startup; with no
-- section the coordinates are NULL and no daylight figure is derived. One
-- bridge instance serves one site, which is what makes the single row correct.
--
-- Also adds solar_daylight(): for a latitude, longitude, date and timezone it
-- returns the daylight length (as an interval and in seconds) and the sunrise
-- and sunset instants. The length is the basis for judging an inverter day's
-- coverage against the daylight actually available (an outage discovered mid-day
-- leaves a half-collected day whose span is far short of sunrise..sunset).
-- Latitude alone fixes the length; longitude, the timezone's UTC offset on the
-- day, and the equation of time place sunrise/sunset on the clock.
--
-- public.site also carries horizon_deg, the sun-centre altitude counted as
-- sunrise/sunset. The -0.833 deg default is geometric (refraction +
-- semidiameter); a real inverter reports a little outside that, so the figure
-- is a per-site calibration knob (lower it to lengthen the daylight window).
--
-- ASCII only: this file is folded into the binary via #embed.
-- =============================================================================

-- Single-row site configuration. The boolean primary key pinned to TRUE
-- (CHECK id) makes a second row impossible, so the bridge can blind-upsert on
-- ON CONFLICT (id) without tracking which row to update.
CREATE TABLE IF NOT EXISTS site (
    id          BOOLEAN          PRIMARY KEY DEFAULT TRUE,
    latitude    DOUBLE PRECISION,
    longitude   DOUBLE PRECISION,
    horizon_deg DOUBLE PRECISION NOT NULL DEFAULT -0.833,
    updated_at  TIMESTAMPTZ      NOT NULL DEFAULT now(),

    CONSTRAINT site_singleton_chk CHECK (id),
    CONSTRAINT site_latitude_chk
        CHECK (latitude  IS NULL OR latitude  BETWEEN  -90 AND  90),
    CONSTRAINT site_longitude_chk
        CHECK (longitude IS NULL OR longitude BETWEEN -180 AND 180),
    CONSTRAINT site_horizon_chk
        CHECK (horizon_deg BETWEEN -18 AND 20)
);

-- -----------------------------------------------------------------------------
-- solar_daylight(lat_deg, lon_deg, for_day, tz, horizon_deg)
--   -> (daylight INTERVAL, seconds INTEGER, sunrise TIMESTAMPTZ, sunset TIMESTAMPTZ)
--
-- Sunrise, sunset and daylight length for a point and date. Solar declination
-- via Cooper's formula and the sunrise hour angle give the length (a function of
-- latitude only); the equation of time, longitude and the timezone's UTC offset
-- on the day (DST-aware) place solar noon -- and thus sunrise/sunset -- on the
-- clock. sunrise/sunset are returned as absolute instants; tz also supplies the
-- offset and defaults to the session TimeZone.
--
-- horizon_deg is the sun-centre altitude at sunrise/sunset (-0.833 deg is the
-- usual refraction + semidiameter convention; raise it to model the higher sun
-- angle at which a PV inverter actually starts/stops producing). At the poles
-- there is no rise/set on the day: daylight is 0 or 24h and sunrise/sunset are
-- NULL.
--
-- STABLE, not IMMUTABLE: the timezone offset depends on the tz database.
-- Accurate to about a minute -- ample for a coverage threshold.
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION solar_daylight(
    lat_deg     DOUBLE PRECISION,
    lon_deg     DOUBLE PRECISION,
    for_day     DATE,
    tz          TEXT DEFAULT current_setting('TimeZone'),
    horizon_deg DOUBLE PRECISION DEFAULT -0.833,
    OUT daylight INTERVAL,
    OUT seconds  INTEGER,
    OUT sunrise  TIMESTAMPTZ,
    OUT sunset   TIMESTAMPTZ)
LANGUAGE sql
STABLE
AS $$
    WITH base AS (
        SELECT radians(lat_deg) AS phi,
               EXTRACT(DOY FROM for_day)::int AS doy,
               -- tz offset (hours) at local noon on for_day, DST-aware
               EXTRACT(epoch FROM (
                   (for_day::timestamp + interval '12 hours')
                   - ((for_day::timestamp + interval '12 hours')
                        AT TIME ZONE tz AT TIME ZONE 'UTC')
               )) / 3600.0 AS off_h
    ),
    sol AS (
        SELECT phi, off_h,
               radians(23.45) * sin(2*pi()/365.0 * (doy + 284)) AS decl,
               radians((360.0/364.0) * (doy - 81))             AS b
        FROM base
    ),
    ang AS (
        SELECT phi, off_h, decl,
               -- equation of time (minutes); places true vs mean solar noon
               9.87*sin(2*b) - 7.53*cos(b) - 1.5*sin(b)        AS eot_min,
               -- cosine of the sunrise/sunset hour angle
               (sin(radians(horizon_deg)) - sin(phi)*sin(decl))
                   / (cos(phi)*cos(decl))                      AS cos_w
        FROM sol
    ),
    t AS (
        SELECT *,
               -- local clock time of solar noon (hours)
               12.0 + off_h - lon_deg/15.0 - eot_min/60.0      AS noon_clock,
               -- daylight length in seconds (0 / 86400 at the poles)
               CASE WHEN cos_w <= -1 THEN 86400
                    WHEN cos_w >=  1 THEN 0
                    ELSE round(2*degrees(acos(cos_w))/15.0*3600.0) END AS day_secs,
               -- half-day length in hours; NULL when the sun never rises/sets
               CASE WHEN cos_w <= -1 OR cos_w >= 1 THEN NULL
                    ELSE degrees(acos(cos_w))/15.0 END          AS half_h
        FROM ang
    ),
    r AS (
        SELECT day_secs, noon_clock, half_h,
               CASE WHEN half_h IS NULL THEN NULL
                    ELSE (for_day::timestamp + make_interval(
                            secs => round((noon_clock - half_h)*3600.0)))
                         AT TIME ZONE tz END                    AS rise
        FROM t
    )
    -- sunset = sunrise + daylight keeps the three outputs mutually consistent
    -- (whole seconds, no independent rounding drift)
    SELECT make_interval(secs => day_secs),
           day_secs::int,
           rise,
           CASE WHEN rise IS NULL THEN NULL
                ELSE rise + make_interval(secs => day_secs) END
    FROM r;
$$;
