# syntax=docker/dockerfile:1

ARG TIMESCALEDB_TAG=2.28.3-pg17

# ---- pg_cron build stage ---------------------------------------------------
FROM timescale/timescaledb:${TIMESCALEDB_TAG} AS builder

# The light TimescaleDB image is postgres:17-alpine underneath, so pg_config
# and the full server headers are already present and pg_cron builds via PGXS.
# It is not packaged for Alpine, hence the pinned source build.
# hadolint ignore=DL3018
RUN apk add --no-cache build-base git

ARG PG_CRON_VERSION=v1.6.7
RUN git clone --branch "${PG_CRON_VERSION}" --depth 1 \
        https://github.com/citusdata/pg_cron.git /build/pg_cron \
    && make -C /build/pg_cron with_llvm=no \
    && make -C /build/pg_cron install with_llvm=no

# ---- Runtime ---------------------------------------------------------------
FROM timescale/timescaledb:${TIMESCALEDB_TAG}

COPY --from=builder /usr/local/lib/postgresql/pg_cron.so \
    /usr/local/lib/postgresql/
COPY --from=builder /usr/local/share/postgresql/extension/pg_cron* \
    /usr/local/share/postgresql/extension/

COPY entrypoint.sh /usr/local/bin/fronius-entrypoint.sh
COPY 900_fronius_cron.sh /docker-entrypoint-initdb.d/

ENTRYPOINT ["fronius-entrypoint.sh"]
CMD ["postgres"]
