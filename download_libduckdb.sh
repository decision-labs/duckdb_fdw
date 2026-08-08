#!/bin/bash
set -euo pipefail

DEFAULT_DUCKDB_VERSION="1.5.5"

normalize_version_tag() {
    case "$1" in
        v*) echo "$1" ;;
        *) echo "v$1" ;;
    esac
}

get_system_info() {
    OS=$(uname -s)
    ARCH=$(uname -m)

    case "$OS" in
        "Darwin")
            PLATFORM="osx"
            ARCH="universal"
            LIB_EXT="dylib"
            ;;
        "Linux")
            PLATFORM="linux"
            case "$ARCH" in
                "x86_64")
                    ARCH="amd64"
                    ;;
                # DuckDB liberleases used "aarch64" through v1.2.x and
                # "arm64" from v1.3.0 onward. Prefer arm64 for current pins.
                "aarch64"|"arm64")
                    ARCH="arm64"
                    ;;
            esac
            LIB_EXT="so"
            ;;
        MINGW*|CYGWIN*|MSYS*)
            PLATFORM="windows"
            ARCH="amd64"
            LIB_EXT="dll"
            ;;
        *)
            echo "Unsupported operating system: $OS" >&2
            exit 1
            ;;
    esac
}

get_system_info

REQUESTED_VERSION=${DUCKDB_VERSION:-$DEFAULT_DUCKDB_VERSION}
VERSION=$(normalize_version_tag "$REQUESTED_VERSION")

try_download() {
    local url="$1"
    echo "Downloading DuckDB ${VERSION} for ${PLATFORM}-${ARCH}..."
    echo "URL: ${url}"
    if ! curl -fsSL -o duckdb-temp.zip "${url}"; then
        rm -f duckdb-temp.zip
        return 1
    fi
    unzip -o duckdb-temp.zip
    rm -f duckdb-temp.zip
    return 0
}

DOWNLOAD_URL="https://github.com/duckdb/duckdb/releases/download/${VERSION}/libduckdb-${PLATFORM}-${ARCH}.zip"

if ! try_download "${DOWNLOAD_URL}"; then
    if [ "${PLATFORM}" = "linux" ] && [ "${ARCH}" = "arm64" ]; then
        ARCH="aarch64"
        FALLBACK_URL="https://github.com/duckdb/duckdb/releases/download/${VERSION}/libduckdb-${PLATFORM}-${ARCH}.zip"
        echo "Primary URL failed; retrying with legacy arch name aarch64..."
        if ! try_download "${FALLBACK_URL}"; then
            echo "Failed to download libduckdb for linux arm (tried arm64 and aarch64)" >&2
            exit 1
        fi
    else
        echo "Failed to download libduckdb from ${DOWNLOAD_URL}" >&2
        exit 1
    fi
fi

if [ ! -f duckdb.h ]; then
    echo "download_libduckdb.sh: duckdb.h missing after extract" >&2
    ls -la >&2 || true
    exit 1
fi

echo "DuckDB ${VERSION} ready (libduckdb.${LIB_EXT}, duckdb.h present)."
