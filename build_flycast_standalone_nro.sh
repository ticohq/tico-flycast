#!/bin/bash
set -e

# Setup paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLYCAST_DIR="$SCRIPT_DIR"
BUILD_DIR="$FLYCAST_DIR/build_tico_standalone"

# NVK (Mesa Vulkan driver for Switch). Same default as the tico libretro NRO.
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"

echo "=== Building tico-flycast standalone (Vulkan) ==="
echo "Source: $FLYCAST_DIR"
echo "Build:  $BUILD_DIR"
echo "NVK:    $MESA_NVK_DIR"

if [ ! -d "$FLYCAST_DIR" ]; then
    echo "Error: Flycast source directory not found at $FLYCAST_DIR"
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure Flycast as a native Switch executable using NVK directly.
# LIBRETRO=OFF removes the libretro frontend layer; USE_TICO keeps the
# Switch/Tico runtime paths and Vulkan/NVK wiring.
cmake "$FLYCAST_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DPLATFORM=libnx \
    -DLIBRETRO=OFF \
    -DUSE_TICO=ON \
    -DNINTENDO_SWITCH=ON \
    -DMESA_NVK_DIR="$MESA_NVK_DIR" \
    -DCMAKE_BUILD_TYPE=Release

# Build
echo "Running Make..."
make -j$(nproc)

echo ""
echo "=== Build Complete ==="

if [ -f "tico-flycast-standalone.nro" ]; then
    echo "SUCCESS: tico-flycast-standalone.nro generated."
    echo "Path: $BUILD_DIR/tico-flycast-standalone.nro"

    cp "tico-flycast-standalone.nro" "$SCRIPT_DIR/tico-flycast-standalone.nro"
    echo "Copied to: $SCRIPT_DIR/tico-flycast-standalone.nro"
else
    echo "ERROR: No standalone .nro file was created."
    echo "Checking for other outputs..."
    ls -la *.nro 2>/dev/null || echo "No .nro files found"
    ls -la *.elf 2>/dev/null || echo "No .elf files found"
    exit 1
fi
