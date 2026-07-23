# syntax=docker/dockerfile:1

ARG ALPINE_VERSION=3.23

# ---- Build stage -----------------------------------------------------------
FROM alpine:${ALPINE_VERSION} AS builder

# Alpine 3.23 ships GCC 15, the minimum for the #embed directive used to fold
# the SQL migrations into the binary.
# hadolint ignore=DL3018
RUN apk add --no-cache \
    build-base cmake samurai git pkgconf \
    libmodbus-dev mosquitto-dev libpq-dev openssl-dev \
    yaml-cpp-dev spdlog-dev fmt-dev nlohmann-json

# CLI11 is not packaged in the Alpine repositories; it is header-only, so a
# pinned checkout installed into /usr/local is all find_package needs.
ARG CLI11_VERSION=v2.5.0
RUN git clone --branch "${CLI11_VERSION}" --depth 1 \
        https://github.com/CLIUtils/CLI11.git /src/cli11 \
    && cmake -S /src/cli11 -B /src/cli11/build -G Ninja \
        -DCLI11_BUILD_TESTS=OFF -DCLI11_BUILD_EXAMPLES=OFF \
        -DCLI11_BUILD_DOCS=OFF \
    && cmake --build /src/cli11/build \
    && cmake --install /src/cli11/build

# libfronius is not packaged for Alpine, so it is built from source here. The
# tag is pinned rather than tracking master to keep image builds reproducible;
# bump LIBFRONIUS_VERSION together with the bridge when the library API moves.
ARG LIBFRONIUS_VERSION=v1.3.6
RUN git clone --branch "${LIBFRONIUS_VERSION}" --depth 1 \
        https://github.com/ahpohl/libfronius.git /src/libfronius \
    && cmake -S /src/libfronius -B /src/libfronius/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/libfronius/build \
    && cmake --install /src/libfronius/build

# The .git directory must be part of the build context: the version is derived
# from git describe at configure time and falls back to 0.0.0 without it.
COPY . /src/fronius-bridge
RUN PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
        cmake -S /src/fronius-bridge -B /src/fronius-bridge/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /src/fronius-bridge/build \
    && DESTDIR=/out cmake --install /src/fronius-bridge/build

# ---- Runtime stage ---------------------------------------------------------
FROM alpine:${ALPINE_VERSION}

# tzdata so TZ=Europe/Berlin works; ca-certificates for MQTT over TLS.
# hadolint ignore=DL3018
RUN apk add --no-cache \
    libstdc++ libmodbus mosquitto-libs libpq \
    yaml-cpp spdlog fmt tzdata ca-certificates

COPY --from=builder /out/usr/local/ /usr/local/

# libfronius is linked dynamically, so the runtime stage needs the shared
# object. Only the SONAME and the real file are copied; the .so devel symlink,
# the static archive and the headers stay in the builder. musl searches
# /usr/local/lib by default, so no ldconfig step is required.
COPY --from=builder /usr/local/lib/libfronius.so.* /usr/local/lib/

# Non-root; dialout matches the usual host group of USB serial adapters. If
# the host uses a different gid, override with group_add in the compose file.
RUN adduser -D -H -s /sbin/nologin fronius \
    && addgroup fronius dialout \
    && mkdir -p /etc/fronius-bridge

USER fronius
ENTRYPOINT ["fronius-bridge"]
CMD ["--config", "/etc/fronius-bridge/config.yaml"]
