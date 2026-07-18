#!/bin/sh
set -e

# Runs once, on an empty data volume, against $POSTGRES_DB. The jobs mirror
# the bare-metal setup documented in DEPLOYMENT.md, except that pg_cron lives
# in the bridge database itself, so plain cron.schedule() suffices.
#
# The job commands reference public.compute_site_rollup, which the bridge
# creates when it first connects and migrates; until then the five-minute job
# records an error in cron.job_run_details. That is deliberate: the missing
# migration surfaces as a visible failure instead of being silently deferred.
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<'EOF'
CREATE EXTENSION pg_cron;

-- Finalize the previous day just after the local-midnight rollover.
SELECT cron.schedule(
  'fronius-daily-rollup',
  '3 0 * * *',
  $job$
    SELECT public.compute_site_rollup(
      (now() AT TIME ZONE current_setting('cron.timezone'))::date - 1,
      current_setting('cron.timezone'));
  $job$);

-- Keep the running day fresh.
SELECT cron.schedule(
  'fronius-current-day-rollup',
  '*/5 * * * *',
  $job$
    SELECT public.compute_site_rollup(
      (now() AT TIME ZONE current_setting('cron.timezone'))::date,
      current_setting('cron.timezone'));
  $job$);
EOF
