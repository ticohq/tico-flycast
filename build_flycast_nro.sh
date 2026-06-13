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

if [ -n "$NRO_FILE" ]; then
    echo "SUCCESS: $NRO_FILE generated."
    echo "Path: $BUILD_DIR/$NRO_FILE"
    
    # Copy to top level for easy access
    cp "$NRO_FILE" "$SCRIPT_DIR/flycast.nro"
    echo "Copied to: $SCRIPT_DIR/flycast.nro"
else
    echo "ERROR: No .nro file was created."
    echo "Checking for other outputs..."
    ls -la *.nro 2>/dev/null || echo "No .nro files found"
    ls -la *.elf 2>/dev/null || echo "No .elf files found"
    exit 1
fi
