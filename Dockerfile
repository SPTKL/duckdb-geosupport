# Fully provisioned DuckDB + Geosupport. Build on an amd64 host; no Python.
ARG DUCKDB_VERSION=1.5.6
FROM debian:bookworm-slim@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587 AS build
ARG DUCKDB_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make libcurl4-openssl-dev libssl-dev ca-certificates curl unzip \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY Makefile ./
COPY scripts/native-build-tool.cpp scripts/fetch-duckdb.sh scripts/duckdb-checksums.txt scripts/
COPY src src
COPY vendor vendor
COPY resources resources
COPY catalog catalog
RUN make DUCKDB_VERSION="$DUCKDB_VERSION" fetch && make -j2 DUCKDB_VERSION="$DUCKDB_VERSION" build

FROM debian:bookworm-slim@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587 AS provision
ARG DUCKDB_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libcurl4 libssl3 libstdc++6 \
    && rm -rf /var/lib/apt/lists/*
ARG GEOSUPPORT_RELEASE=26c
ENV GEOSUPPORT_HOME=/opt/geosupport/store GEOSUPPORT_RUNTIME=local
COPY --from=build /source/build/deps/${DUCKDB_VERSION}/duckdb /usr/local/bin/duckdb
COPY --from=build /source/build/native/${DUCKDB_VERSION}/geosupport.duckdb_extension /opt/geosupport/geosupport.duckdb_extension
RUN case "$GEOSUPPORT_RELEASE" in 26b|26c) ;; *) exit 1 ;; esac \
    && mkdir -p /opt/duckdb/extensions \
    && duckdb -unsigned -bail -no-stdin -c "\
       SET extension_directory='/opt/duckdb/extensions'; \
       INSTALL '/opt/geosupport/geosupport.duckdb_extension'; \
       LOAD geosupport; CALL gs_install('$GEOSUPPORT_RELEASE'); CALL gs_verify('$GEOSUPPORT_RELEASE');" \
    && printf "%s\n" "SET extension_directory='/opt/duckdb/extensions';" "LOAD geosupport;" \
       "SET VARIABLE geosupport_release='$GEOSUPPORT_RELEASE';" > /opt/geosupport/init.sql

FROM debian:bookworm-slim@sha256:7c7b2c966bc9ee8cedfeef67e0e279108992c77681fa595db4a9d65c06ccc587
ARG DUCKDB_VERSION
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libcurl4 libssl3 libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home duckdb && mkdir /data && chown duckdb:duckdb /data
ARG GEOSUPPORT_RELEASE=26c
LABEL org.opencontainers.image.title="DuckDB with NYC Geosupport" \
      org.opencontainers.image.description="DuckDB ${DUCKDB_VERSION}, native Geosupport extension, and a verified quarterly engine/data bundle" \
      org.opencontainers.image.source="https://github.com/SPTKL/duckdb-geosupport" \
      org.opencontainers.image.version="0.3.0" \
      org.nycplanning.geosupport.release="$GEOSUPPORT_RELEASE" \
      org.nycplanning.duckdb.version="$DUCKDB_VERSION"
ENV GEOSUPPORT_HOME=/opt/geosupport/store GEOSUPPORT_RUNTIME=local
COPY --from=provision /usr/local/bin/duckdb /usr/local/bin/duckdb
COPY --from=provision --chown=duckdb:duckdb /opt/geosupport /opt/geosupport
COPY --from=provision /opt/duckdb /opt/duckdb
COPY LICENSE THIRD_PARTY_NOTICES.md /usr/share/doc/duckdb-geosupport/
COPY vendor/duckdb/LICENSE /usr/share/doc/duckdb-geosupport/DuckDB-LICENSE
COPY vendor/nlohmann/LICENSE /usr/share/doc/duckdb-geosupport/nlohmann-LICENSE
COPY vendor/miniz/LICENSE /usr/share/doc/duckdb-geosupport/miniz-LICENSE
COPY vendor/picosha2/LICENSE /usr/share/doc/duckdb-geosupport/picosha2-LICENSE
COPY resources/PYTHON_GEOSUPPORT_LICENSE /usr/share/doc/duckdb-geosupport/layouts-LICENSE
USER duckdb
WORKDIR /data
ENTRYPOINT ["duckdb", "-unsigned", "-bail", "-init", "/opt/geosupport/init.sql"]
