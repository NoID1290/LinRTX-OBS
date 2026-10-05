#!/usr/bin/env bash
# ==============================================================================
# LinRTX-OBS Installer Script
# Builds and installs the LinRTX-OBS plugin for OBS Studio on Linux.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

INSTALL_MODE="user"
if [[ "${1:-}" == "--system" ]]; then
    INSTALL_MODE="system"
fi

echo "============================================================"
echo " LinRTX-OBS Plugin Installer"
echo " Target Mode: ${INSTALL_MODE}"
echo "============================================================"

# 1. Check compiler and build tools
echo -n "[1/5] Checking build tools... "
if ! command -v g++ >/dev/null 2>&1; then
    echo "FAILED"
    echo "Error: g++ compiler is required. Please install gcc/g++."
    exit 1
fi

if ! command -v nvcc >/dev/null 2>&1 && [[ ! -x /opt/cuda/bin/nvcc ]]; then
    echo "FAILED"
    echo "Error: NVIDIA CUDA Toolkit (nvcc) is required. Please install CUDA."
    exit 1
fi
echo "OK"

# 2. Check OBS headers & libraries
echo -n "[2/5] Checking libobs development libraries... "
if ! pkg-config --exists libobs && [[ ! -d /usr/include/obs ]]; then
    echo "FAILED"
    echo "Error: libobs development headers not found. Please install obs-studio dev package."
    exit 1
fi
echo "OK"

# 3. Check NVIDIA VFX SDK runtime
echo -n "[3/5] Detecting NVIDIA Video Effects SDK... "
SDK_FOUND=0
POSSIBLE_PATHS=(
    "${LINRTX_SDK_PATH:-}"
    "$HOME/ComfyUI/.venv/lib/python3.12/site-packages/nvvfx/libs"
    "$HOME/ComfyUI/.venv/lib/python3.11/site-packages/nvvfx/libs"
    "$HOME/ComfyUI/.venv/lib/python3.10/site-packages/nvvfx/libs"
    "$HOME/.local/lib/python3.12/site-packages/nvvfx/libs"
    "/usr/local/lib/nvvfx/libs"
    "/opt/nvidia/vfx/lib"
)

for p in "${POSSIBLE_PATHS[@]}"; do
    if [[ -n "$p" && -f "$p/libVideoFX.so" ]]; then
        echo "Found at $p"
        SDK_FOUND=1
        break
    fi
done

if [[ $SDK_FOUND -eq 0 ]]; then
    echo "NOTE"
    echo "Warning: NVIDIA VFX SDK libraries (libVideoFX.so) were not detected at standard locations."
    echo "You can install them via pip: pip install nvidia-vfx --extra-index-url https://pypi.nvidia.com"
    echo "or set LINRTX_SDK_PATH=/path/to/libs."
    echo "Plugin will build and install, but will await SDK runtime before filter activation."
fi

# 4. Build Plugin
echo "[4/5] Building LinRTX-OBS plugin..."
make clean
make -j"$(nproc)"

# 5. Install Plugin
echo "[5/5] Installing LinRTX-OBS..."
if [[ "$INSTALL_MODE" == "user" ]]; then
    USER_DEST="$HOME/.config/obs-studio/plugins/linrtx-obs"
    mkdir -p "${USER_DEST}/bin/64bit" "${USER_DEST}/data"
    cp -f linrtx-obs.so "${USER_DEST}/bin/64bit/"
    cp -rf data/* "${USER_DEST}/data/"
    echo "Successfully installed LinRTX-OBS to: ${USER_DEST}"
else
    echo "Installing system-wide (requires sudo)..."
    sudo mkdir -p /usr/lib/obs-plugins /usr/share/obs/obs-plugins/linrtx-obs
    sudo cp -f linrtx-obs.so /usr/lib/obs-plugins/
    sudo cp -rf data/* /usr/share/obs/obs-plugins/linrtx-obs/
    echo "Successfully installed LinRTX-OBS to: /usr/lib/obs-plugins"
fi

echo "============================================================"
echo " Installation complete! Restart OBS Studio to use LinRTX."
echo " Filters available in OBS: 'NVIDIA RTX Video Super Resolution' & 'NVIDIA RTX HDR (TrueHDR)'"
echo "============================================================"
