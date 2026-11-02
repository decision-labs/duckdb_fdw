#!/bin/bash
#
# download_libduckdb.sh — fetch the prebuilt libduckdb matching this platform.
#
# Fail hard on any error: the previous "curl + unzip and hope" behaviour let
# a 404 (e.g. a wrong asset name) download GitHub's HTML error page, which
# only exploded later at `make` time with "duckdb.h: No such file or
# directory".  set -euo pipefail + curl --fail + a post-download check make
# the failure point the download step itself.

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
                # DuckDB names the Linux ARM64 asset "arm64" (NOT
                # "aarch64"): .../libduckdb-linux-arm64.zip.  The
                # aarch64 name 404s and silently breaks the build on
                # ubuntu-*-arm runners. Prefer arm64 for current pins.
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

# DuckDB 2.0+ changed its release asset naming: instead of
# libduckdb-${PLATFORM}-${ARCH}.zip it ships duckdb-shared-libs-${PLATFORM}-${ARCH}
# .tar.gz (the PLATFORM/ARCH suffixes are unchanged, e.g. linux-amd64,
# osx-universal, windows-amd64).  Detect the major version and pick the
# matching asset base name + archive format.
MAJOR="${REQUESTED_VERSION#v}"
MAJOR="${MAJOR%%.*}"

if [ "$MAJOR" -ge 2 ] 2>/dev/null; then
    ASSET_BASE="duckdb-shared-libs"
    ASSET_EXT="tar.gz"
    EXTRACT="tar xzf"
else
    ASSET_BASE="libduckdb"
    ASSET_EXT="zip"
    EXTRACT="unzip -o"
fi

try_download() {
    local url="$1"
    local file="$2"
    echo "Downloading DuckDB ${VERSION} for ${PLATFORM}-${ARCH}..."
    echo "URL: ${url}"
    if ! curl --fail -L --retry 3 -o "$file" "${url}"; then
        rm -f "$file"
        return 1
    fi
    $EXTRACT "$file"
    rm -f "$file"
    return 0
}

DOWNLOAD_URL="https://github.com/duckdb/duckdb/releases/download/${VERSION}/${ASSET_BASE}-${PLATFORM}-${ARCH}.${ASSET_EXT}"
DOWNLOAD_FILE="duckdb-temp.${ASSET_EXT}"

if ! try_download "${DOWNLOAD_URL}" "${DOWNLOAD_FILE}"; then
    if [ "${PLATFORM}" = "linux" ] && [ "${ARCH}" = "arm64" ] && [ "$MAJOR" -lt 2 ] 2>/dev/null; then
        ARCH="aarch64"
        FALLBACK_URL="https://github.com/duckdb/duckdb/releases/download/${VERSION}/libduckdb-${PLATFORM}-${ARCH}.zip"
        echo "Primary URL failed; retrying with legacy arch name aarch64..."
        if ! try_download "${FALLBACK_URL}" "duckdb-temp.zip"; then
            echo "Failed to download libduckdb for linux arm (tried arm64 and aarch64)" >&2
            exit 1
        fi
    else
        echo "Failed to download libduckdb from ${DOWNLOAD_URL}" >&2
        exit 1
    fi
fi

if [ ! -f duckdb.h ]; then
    echo "ERROR: download did not produce duckdb.h — is ${DOWNLOAD_URL} valid?" >&2
    echo "       (404 assets yield an HTML page; check the asset name for ${PLATFORM}-${ARCH})" >&2
    exit 1
fi
case "${PLATFORM}-${ARCH}" in
    osx-universal)
        [ -f libduckdb.dylib ] || { echo "ERROR: libduckdb.dylib missing after download" >&2; exit 1; }
        ;;
    *)
        [ -f "libduckdb.${LIB_EXT}" ] || { echo "ERROR: libduckdb.${LIB_EXT} missing after download" >&2; exit 1; }
        ;;
esac

echo "Downloaded DuckDB ${VERSION}: duckdb.h + libduckdb.${LIB_EXT}"
