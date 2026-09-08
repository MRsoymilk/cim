FROM ubuntu:24.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libsqlite3-dev \
        libsodium-dev \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B /build -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DCIM_BUILD_CLIENT=OFF \
    && cmake --build /build --target cim-server --parallel

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libsqlite3-0 \
        libsodium23 \
        libssl3t64 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 cim \
    && useradd --uid 10001 --gid cim --no-create-home --shell /usr/sbin/nologin cim \
    && install -d --owner=cim --group=cim --mode=0700 /data

COPY --from=builder /build/cim-server /app/cim-server

USER 10001:10001
WORKDIR /data
VOLUME ["/data"]
EXPOSE 9001

HEALTHCHECK --interval=30s --timeout=8s --start-period=10s --retries=3 \
    CMD ["/app/cim-server", "users"]

ENTRYPOINT ["/app/cim-server"]
CMD ["--tls-cert", "/certs/server-chain.crt", "--tls-key", "/certs/server.key"]
