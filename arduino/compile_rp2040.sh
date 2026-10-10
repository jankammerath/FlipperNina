#!/usr/bin/env bash
#
# Compile an Arduino sketch (.ino) to a .uf2 file for the
# Arduino Nano RP2040 Connect using arduino-cli.
#
# Usage:
#   ./compile_rp2040.sh FlipperNina_Arduino.ino FlipperNina_RP2040.uf2
#   ./compile_rp2040.sh FlipperNina_Arduino.ino ./dist/FlipperNina_RP2040.uf2

set -euo pipefail

SKETCH_PATH="${1:-}"
OUTPUT_TARGET="${2:-FlipperNina_RP2040.uf2}"

BOARD_PACKAGE="arduino:mbed_nano"
BOARD_FQBN="arduino:mbed_nano:nanorp2040connect"

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

# 2. Resolve output path properly relative to current working directory
if [[ "$OUTPUT_TARGET" == *.uf2 ]]; then
  TARGET_DIR="$(dirname "$OUTPUT_TARGET")"
  TARGET_FILE="$(basename "$OUTPUT_TARGET")"
  mkdir -p "$TARGET_DIR"
  FINAL_UF2_DIR="$(cd "$TARGET_DIR" && pwd)"
  FINAL_UF2_PATH="$FINAL_UF2_DIR/$TARGET_FILE"
else
  mkdir -p "$OUTPUT_TARGET"
  FINAL_UF2_DIR="$(cd "$OUTPUT_TARGET" && pwd)"
  FINAL_UF2_PATH="$FINAL_UF2_DIR/${SKETCH_NAME}.uf2"
fi

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

if ! arduino-cli core list | grep -q "$BOARD_PACKAGE"; then
  echo "Installing '$BOARD_PACKAGE' core..."
  arduino-cli core install "$BOARD_PACKAGE"
fi

# 4. Compile sketch
echo "=== Compiling Sketch ==="
echo "Sketch:  $SKETCH_ABS"
echo "Board:   $BOARD_FQBN"
echo "Target:  $FINAL_UF2_PATH"

arduino-cli compile \
  --fqbn "$BOARD_FQBN" \
  --build-path "$BUILD_DIR" \
  "$COMPILE_DIR"

# 5. Extract or convert UF2
PRODUCED_UF2=""
for candidate in \
  "$BUILD_DIR/${SKETCH_NAME}.ino.uf2" \
  "$BUILD_DIR/${SKETCH_NAME}.uf2" \
  "$BUILD_DIR"/*.uf2; do
  if [[ -f "$candidate" ]]; then
    PRODUCED_UF2="$candidate"
    break
  fi
done

if [[ -n "$PRODUCED_UF2" ]]; then
  cp "$PRODUCED_UF2" "$FINAL_UF2_PATH"
  echo "UF2 binary ready: $FINAL_UF2_PATH"
else
  ELF_FILE="$BUILD_DIR/${SKETCH_NAME}.ino.elf"
  BIN_FILE="$BUILD_DIR/${SKETCH_NAME}.ino.bin"

  echo "Direct UF2 not produced. Converting binary..."
  if command -v picotool &>/dev/null && [[ -f "$ELF_FILE" ]]; then
    picotool uf2 convert "$ELF_FILE" "$FINAL_UF2_PATH"
  elif command -v elf2uf2 &>/dev/null && [[ -f "$ELF_FILE" ]]; then
    elf2uf2 "$ELF_FILE" "$FINAL_UF2_PATH"
  elif command -v uf2conv.py &>/dev/null && [[ -f "$BIN_FILE" ]]; then
    python3 "$(command -v uf2conv.py)" -c -b 0x10000000 -f 0xe48bff56 "$BIN_FILE" -o "$FINAL_UF2_PATH"
  else
    echo "Error: Compilation finished, but UF2 converter (picotool, elf2uf2, uf2conv.py) was not found."
    exit 1
  fi
  echo "Converted UF2 binary ready: $FINAL_UF2_PATH"
fi

echo "=== Done ==="