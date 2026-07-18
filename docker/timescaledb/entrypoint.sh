#!/bin/sh
set -e

# Inject the pg_cron settings as command-line -c flags instead of appending
# postgresql.conf from an init script: the flags also reach the temporary
# server the entrypoint runs during initdb, so the first-boot script can
# create the extension and the rollup jobs in a single pass. Command-line
# settings take precedence over the shared_preload_libraries line written by
# timescaledb-tune. Background workers avoid the localhost libpq connection
# pg_cron would otherwise make, which the image's scram host auth rejects.
if [ "$1" = "postgres" ]; then
    set -- "$@" \
        -c "shared_preload_libraries=timescaledb,pg_cron" \
        -c "cron.database_name=${POSTGRES_DB:-postgres}" \
        -c "cron.timezone=${TZ:-Etc/UTC}" \
        -c "cron.use_background_workers=on"
fi

exec docker-entrypoint.sh "$@"
