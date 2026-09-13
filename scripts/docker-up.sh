#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname -- "$SCRIPT_DIR")"
COMPOSE=(docker compose --project-directory "$PROJECT_DIR" -f "$PROJECT_DIR/compose.yaml")

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    printf 'Usage: bash scripts/docker-up.sh [docker compose up options]\n'
    printf 'Initializes local TLS, data and shared networking, then builds and starts CIM.\n'
    exit 0
fi

for tool in docker git openssl; do
    command -v "$tool" >/dev/null || { printf 'ERROR: %s is required.\n' "$tool" >&2; exit 1; }
done
if (( EUID == 0 )); then
    printf 'ERROR: Run as a non-root user with Docker access, without sudo.\n' >&2
    exit 1
fi

export CIM_UID="${CIM_UID:-$(id -u)}"
export CIM_GID="${CIM_GID:-$(id -g)}"
export CIM_DATA_DIR="${CIM_DATA_DIR:-$PROJECT_DIR/docker-data}"
export CIM_TLS_DIR="${CIM_TLS_DIR:-$PROJECT_DIR/tls}"
# Match Compose's project-relative bind paths even when invoked elsewhere.
[[ "$CIM_DATA_DIR" == /* ]] || CIM_DATA_DIR="$PROJECT_DIR/$CIM_DATA_DIR"
[[ "$CIM_TLS_DIR" == /* ]] || CIM_TLS_DIR="$PROJECT_DIR/$CIM_TLS_DIR"

"${COMPOSE[@]}" config --quiet

# Listing first distinguishes a missing network from an unavailable daemon.
networks="$(docker network ls --format '{{.Name}}')"
if ! [[ $'\n'"$networks"$'\n' == *$'\n'dev-net$'\n'* ]]; then
    if ! docker network create --driver bridge \
        --opt com.docker.network.bridge.name=br-docker dev-net; then
        # Another project may have created the shared network concurrently.
        docker network inspect dev-net >/dev/null
    fi
fi

driver="$(docker network inspect --format '{{.Driver}}' dev-net)"
if [[ "$driver" != bridge ]]; then
    printf "ERROR: Docker network 'dev-net' uses driver '%s'; expected 'bridge'.\n" "$driver" >&2
    exit 1
fi

git -C "$PROJECT_DIR" submodule update --init --recursive
mkdir -p -- "$CIM_DATA_DIR"
if [[ ! -e "$CIM_TLS_DIR/server-chain.crt" && ! -e "$CIM_TLS_DIR/server.key" ]]; then
    bash "$SCRIPT_DIR/generate-tls-cert.sh" --output-dir "$CIM_TLS_DIR" \
        --dns localhost --ip 127.0.0.1 --ip ::1
fi
if [[ ! -r "$CIM_TLS_DIR/server-chain.crt" || ! -r "$CIM_TLS_DIR/server.key" ]]; then
    printf 'ERROR: CIM_TLS_DIR must contain readable server-chain.crt and server.key.\n' >&2
    exit 1
fi

printf '[Docker] Using shared network: dev-net\n'
printf '[Docker] TLS directory: %s (clients must trust its CA certificate)\n' "$CIM_TLS_DIR"
exec "${COMPOSE[@]}" up -d --build "$@"
