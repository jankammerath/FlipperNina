#!/usr/bin/env bash
#
# Compile an Arduino sketch (.ino) to a .bin file for the
# Arduino Nano ESP32 (NORA-W106) using arduino-cli.
#
# Usage:
#   ./compile_nanoesp32.sh Flipper_ArduinoNanoESP32.ino FlipperNina_NanoESP32.bin
#   ./compile_nanoesp32.sh Flipper_ArduinoNanoESP32.ino ./dist/
#
# Produces the application image (flash with `arduino-cli upload` / dfu-util) and,
# if the core provides it, a <name>.merged.bin with bootloader + partitions for esptool at 0x0.

set -euo pipefail

SKETCH_PATH="${1:-}"
OUTPUT_TARGET="${2:-FlipperNina_NanoESP32.bin}"

BOARD_PACKAGE="arduino:esp32"
BOARD_FQBN="arduino:esp32:nano_nora"

# 1. Validation
if [[ -z "$SKETCH_PATH" ]]; then
  echo "Error: Missing sketch path."
  echo "Usage: $0 <path_to_sketch.ino> [output_file_or_dir]"
  exit 1
fi

if [[ ! -f "$SKETCH_PATH" ]]; then
  echo "Error: Sketch file not found: $SKETCH_PATH"
  exit 1
fi

SKETCH_ABS="$(cd "$(dirname "$SKETCH_PATH")" && pwd)/$(basename "$SKETCH_PATH")"
SKETCH_SRC_DIR="$(dirname "$SKETCH_ABS")"
SKETCH_FILE="$(basename "$SKETCH_PATH")"
SKETCH_NAME="${SKETCH_FILE%.ino}"

# 2. Resolve output path relative to the current working directory
if [[ "$OUTPUT_TARGET" == *.bin ]]; then
  TARGET_DIR="$(dirname "$OUTPUT_TARGET")"
  TARGET_FILE="$(basename "$OUTPUT_TARGET")"
  mkdir -p "$TARGET_DIR"
  FINAL_BIN_DIR="$(cd "$TARGET_DIR" && pwd)"
  FINAL_BIN_PATH="$FINAL_BIN_DIR/$TARGET_FILE"
else
  mkdir -p "$OUTPUT_TARGET"
  FINAL_BIN_DIR="$(cd "$OUTPUT_TARGET" && pwd)"
  FINAL_BIN_PATH="$FINAL_BIN_DIR/${SKETCH_NAME}.bin"
fi
FINAL_MERGED_PATH="${FINAL_BIN_PATH%.bin}.merged.bin"

TEMP_STAGE_DIR=""
BUILD_DIR=""
cleanup() {
  [[ -n "$TEMP_STAGE_DIR" && -d "$TEMP_STAGE_DIR" ]] && rm -rf "$TEMP_STAGE_DIR"
  [[ -n "$BUILD_DIR" && -d "$BUILD_DIR" ]] && rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

# 3. Handle Arduino folder matching
if [[ "$(basename "$SKETCH_SRC_DIR")" != "$SKETCH_NAME" ]]; then
  TEMP_STAGE_DIR="$(mktemp -d)"
  COMPILE_DIR="$TEMP_STAGE_DIR/$SKETCH_NAME"
  mkdir -p "$COMPILE_DIR"
  # Copy helper sources but no other .ino files, which would be merged into this sketch.
  find "$SKETCH_SRC_DIR" -maxdepth 1 -type f \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cpp' \) \
    -exec cp {} "$COMPILE_DIR/" \;
  cp "$SKETCH_ABS" "$COMPILE_DIR/$SKETCH_FILE"
else
  COMPILE_DIR="$SKETCH_SRC_DIR"
fi

BUILD_DIR="$(mktemp -d)"

echo "=== Preparing Environment ==="
arduino-cli core update-index >/dev/null 2>&1 || true

if ! arduino-cli core list | grep -q "^$BOARD_PACKAGE "; then
  echo "Installing '$BOARD_PACKAGE' core..."
  arduino-cli core install "$BOARD_PACKAGE"
fi

# 4. Compile sketch
echo "=== Compiling Sketch ==="
echo "Sketch:  $SKETCH_ABS"
echo "Board:   $BOARD_FQBN"
echo "Target:  $FINAL_BIN_PATH"

arduino-cli compile \
  --fqbn "$BOARD_FQBN" \
  --build-path "$BUILD_DIR" \
  "$COMPILE_DIR"

# 5. Copy binaries
APP_BIN="$BUILD_DIR/${SKETCH_NAME}.ino.bin"
MERGED_BIN="$BUILD_DIR/${SKETCH_NAME}.ino.merged.bin"

if [[ ! -f "$APP_BIN" ]]; then
  echo "Error: Compilation finished, but $APP_BIN was not produced."
  exit 1
fi

cp "$APP_BIN" "$FINAL_BIN_PATH"
echo "App binary ready:    $FINAL_BIN_PATH"

if [[ -f "$MERGED_BIN" ]]; then
  cp "$MERGED_BIN" "$FINAL_MERGED_PATH"
  echo "Merged binary ready: $FINAL_MERGED_PATH"
fi

echo "=== Done ==="
