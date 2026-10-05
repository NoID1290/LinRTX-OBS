#!/usr/bin/env bash
# ==============================================================================
# LinRTX-OBS Uninstaller Script
# Removes LinRTX-OBS plugin files from the system or user directory.
# ==============================================================================

set -euo pipefail

MODE="user"
if [[ "${1:-}" == "--system" ]]; then
    MODE="system"
fi

echo "============================================================"
echo " LinRTX-OBS Uninstaller"
echo " Target Mode: ${MODE}"
echo "============================================================"

if [[ "$MODE" == "user" ]]; then
    TARGET_DIR="$HOME/.config/obs-studio/plugins/linrtx-obs"
    if [[ -d "$TARGET_DIR" ]]; then
        rm -rf "$TARGET_DIR"
        echo "Removed user plugin directory: $TARGET_DIR"
    else
        echo "User plugin directory not found ($TARGET_DIR). Nothing to remove."
    fi
else
    echo "Removing system-wide installation (requires sudo)..."
    if [[ -f /usr/lib/obs-plugins/linrtx-obs.so ]]; then
        sudo rm -f /usr/lib/obs-plugins/linrtx-obs.so
        echo "Removed /usr/lib/obs-plugins/linrtx-obs.so"
    fi
    if [[ -d /usr/share/obs/obs-plugins/linrtx-obs ]]; then
        sudo rm -rf /usr/share/obs/obs-plugins/linrtx-obs
        echo "Removed /usr/share/obs/obs-plugins/linrtx-obs"
    fi
fi

echo "LinRTX-OBS uninstallation complete."
