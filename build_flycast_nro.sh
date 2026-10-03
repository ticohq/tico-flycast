#!/bin/bash
set -e

# Setup paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLYCAST_DIR="$SCRIPT_DIR"
BUILD_DIR="$FLYCAST_DIR/build_tico"

# NVK (Mesa Vulkan driver for Switch). Same default as dolphin's standalone NRO.
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"

echo "=== Building tico-flycast (Vulkan) ==="
echo "Source: $FLYCAST_DIR"
echo "Build:  $BUILD_DIR"
echo "NVK:    $MESA_NVK_DIR"

if [ ! -d "$FLYCAST_DIR" ]; then
    echo "Error: Flycast source directory not found at $FLYCAST_DIR"
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure for Switch with the tico Vulkan frontend.
# LIBRETRO=ON enables libretro core building, USE_TICO=ON wires in the
# tico-flycast.nro target. USE_VULKAN is forced ON inside CMakeLists.txt
# for this combination — no need to pass it here.
cmake "$FLYCAST_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DPLATFORM=libnx \
    -DLIBRETRO=ON \
    -DUSE_TICO=ON \
    -DNINTENDO_SWITCH=ON \
    -DMESA_NVK_DIR="$MESA_NVK_DIR" \
    -DDISABLE_LOGGING=ON \
    -DCMAKE_BUILD_TYPE=Release

# Build
echo "Running Make..."
make -j$(nproc)

echo ""
echo "=== Build Complete ==="

# Check for NRO output (might be named based on output_name or target name)
NRO_FILE=""
if [ -f "tico-flycast.nro" ]; then
    NRO_FILE="tico-flycast.nro"
elif [ -f "flycast_libretro.nro" ]; then
    NRO_FILE="flycast_libretro.nro"
fi

if [ -z "$NRO_FILE" ]; then
    echo "ERROR: No .nro file was created."
    echo "Checking for other outputs..."
    ls -la *.nro 2>/dev/null || echo "No .nro files found"
    ls -la *.elf 2>/dev/null || echo "No .elf files found"
    exit 1
fi

# Copy to top level for easy access
cp "$NRO_FILE" "$SCRIPT_DIR/flycast.nro"

#---------------------------------------------------------------------------------
# Module bundle
#
# A module is a directory, not a bare NRO: tico discovers it by reading
# module.json, and everything the module owns -- its settings definition,
# gamelists and console artwork -- travels with it. romfs is per-NRO, so tico
# cannot read any of this out of the core; the bundle is what carries it.
#
# The NRO sits beside module.json, so an installed bundle is self-contained and
# extracts straight into sdmc:/tico/modules/<id>/.
#---------------------------------------------------------------------------------
MODULE_SRC="$FLYCAST_DIR/tico/module"
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_NRO=$(sed -n 's/.*"nro"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$BUILD_DIR/module/$MODULE_ID"

rm -rf "$BUILD_DIR/module"
mkdir -p "$MODULE_OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp "$NRO_FILE" "$MODULE_OUT/$MODULE_NRO"

# Tico prefers .json.gz when resolving a gamelist.
if [ -d "$MODULE_OUT/gamelists" ]; then
    gzip -f -9 "$MODULE_OUT"/gamelists/*.json 2>/dev/null || true
fi

BUNDLE="$BUILD_DIR/tico-$MODULE_ID-module.zip"
rm -f "$BUNDLE"
( cd "$BUILD_DIR/module" && zip -qr "$BUNDLE" "$MODULE_ID" )
cp "$BUNDLE" "$SCRIPT_DIR/"

echo "======================================"
echo "Build successful!"
echo "  NRO:    $BUILD_DIR/$NRO_FILE"
echo "  Module: $BUNDLE"
echo "          extracts to sdmc:/tico/modules/$MODULE_ID/"
echo "======================================"
find "$MODULE_OUT" -type f | sed "s|$BUILD_DIR/module/|    |"
