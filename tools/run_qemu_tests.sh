#!/usr/bin/env bash

set -euo pipefail

# Resolve script directory to execute relative paths correctly
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

QEMU_RELEASE="esp-develop-9.2.2-20260417"
QEMU_DIST="qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz"
QEMU_SHA256="0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35"
QEMU_URL="https://github.com/espressif/qemu/releases/download/${QEMU_RELEASE}/${QEMU_DIST}"
TOOLS_DIR="${ESP_MINER_TOOLS_DIR:-$(dirname "$ROOT_DIR")/.tools}"
QEMU_DIR="$TOOLS_DIR/esp-qemu/$QEMU_RELEASE"
QEMU_BIN="${ESP_QEMU_BIN:-$QEMU_DIR/bin/qemu-system-xtensa}"
BUILD_DIR="${ESP_QEMU_BUILD_DIR:-$ROOT_DIR/.cache/qemu-test-build}"

echo "=== ESP-Miner QEMU Test Runner ==="

# Check if ESP-IDF environment is already sourced
if ! command -v idf.py &> /dev/null; then
    echo "ESP-IDF environment not detected in PATH."
    # Try common local installation paths
    IDF_EXPORT_PATHS=(
        "$HOME/esp/esp-idf-v5.5.3/export.sh"
        "$HOME/esp/esp-idf-v5.5.4/export.sh"
        "$HOME/esp/v5.5.1/esp-idf/export.sh"
        "$HOME/esp/v5.5/esp-idf/export.sh"
        "$HOME/esp/esp-idf/export.sh"
    )
    
    SOURCED=false
    for export_path in "${IDF_EXPORT_PATHS[@]}"; do
        if [ -f "$export_path" ]; then
            echo "Found ESP-IDF export script at: $export_path"
            echo "Sourcing ESP-IDF environment..."
            . "$export_path"
            SOURCED=true
            break
        fi
    done
    
    if [ "$SOURCED" = false ]; then
        echo "ERROR: Could not locate ESP-IDF export script."
        echo "Please source it manually (e.g. '. ~/esp/v5.5.1/esp-idf/export.sh') before running this script."
        exit 1
    fi
fi

# Use a QEMU already in PATH when available. Otherwise install the exact,
# checksum-pinned Espressif release used by esp32-qemu-test-action.
if command -v qemu-system-xtensa &> /dev/null; then
    QEMU_BIN="$(command -v qemu-system-xtensa)"
elif [ ! -x "$QEMU_BIN" ]; then
    echo "Installing Espressif QEMU $QEMU_RELEASE..."
    mkdir -p "$QEMU_DIR"
    archive="$(mktemp --suffix=.tar.xz)"
    trap 'rm -f "$archive"' EXIT
    curl --fail --location --silent --show-error "$QEMU_URL" --output "$archive"
    printf '%s  %s\n' "$QEMU_SHA256" "$archive" | sha256sum --check --strict
    tar -xf "$archive" -C "$QEMU_DIR" --strip-components=1
    rm -f "$archive"
    trap - EXIT
fi

"$QEMU_BIN" --version | head -n 1

echo "Building test-ci project..."
cd "$ROOT_DIR/test-ci"
CCACHE_DIR="${CCACHE_DIR:-$ROOT_DIR/.cache/qemu-test-ccache}" \
    IDF_TARGET=esp32s3 \
    idf.py -B "$BUILD_DIR" build

echo "Merging binaries..."
cd "$BUILD_DIR"
esptool.py --chip esp32s3 merge_bin --fill-flash-size 16MB -o flash_image.bin @flash_args

echo "Running tests in QEMU emulator..."
output_log="$PWD/output.log"
rm -f "$output_log"
timeout "${QEMU_TIMEOUT:-5m}" "$QEMU_BIN" \
    -machine esp32s3 \
    -monitor none \
    -nographic \
    -no-reboot \
    -watchdog-action shutdown \
    -drive file=flash_image.bin,if=mtd,format=raw \
    -m 4 \
    -serial "file:$output_log"

cat "$output_log"

summary="$(tr -d '\r' < "$output_log" | grep -E '[[:digit:]]+ Tests [[:digit:]]+ Failures [[:digit:]]+ Ignored' | tail -n 1 || true)"
if [ -z "$summary" ]; then
    echo "ERROR: QEMU output did not contain a Unity test summary." >&2
    exit 1
fi

failures="$(printf '%s\n' "$summary" | sed -E 's/.* Tests ([0-9]+) Failures.*/\1/')"
printf '\nQEMU summary: %s\n' "$summary"
if [ "$failures" -ne 0 ]; then
    exit 1
fi
