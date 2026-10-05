#!/usr/bin/env bash
set -euo pipefail

echo "=========================================================="
echo " LinRTX-OBS Environment Verification"
echo "=========================================================="

# 1. Distro & Kernel
echo -n "[1/7] Distro & Kernel: "
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "$PRETTY_NAME (Kernel $(uname -r))"
else
    uname -a
fi

# 2. NVIDIA Driver & GPU
echo -n "[2/7] NVIDIA Driver & GPU: "
if command -v nvidia-smi >/dev/null 2>&1; then
    DRIVER_VER=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -n 1)
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -n 1)
    COMPUTE_CAP=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -n 1)
    echo "$GPU_NAME (Driver $DRIVER_VER, Compute Capability sm_$COMPUTE_CAP)"
else
    echo "ERROR: nvidia-smi not found!"
    exit 1
fi

# 3. CUDA Toolkit
echo -n "[3/7] CUDA Toolkit: "
if command -v nvcc >/dev/null 2>&1; then
    NVCC_VER=$(nvcc --version | grep "release" | sed -n 's/.*release \([0-9.]*\).*/\1/p')
    echo "Found nvcc ($NVCC_VER at $(command -v nvcc))"
elif [ -x /opt/cuda/bin/nvcc ]; then
    NVCC_VER=$(/opt/cuda/bin/nvcc --version | grep "release" | sed -n 's/.*release \([0-9.]*\).*/\1/p')
    echo "Found nvcc ($NVCC_VER at /opt/cuda/bin/nvcc)"
else
    echo "WARNING: nvcc not found on PATH or /opt/cuda/bin/nvcc"
fi

# 4. OBS Studio & Headers
echo -n "[4/7] OBS Studio: "
if command -v obs >/dev/null 2>&1; then
    OBS_VER=$(obs --version 2>&1 | head -n 1)
    echo "$OBS_VER ($(command -v obs))"
else
    echo "ERROR: obs binary not found!"
    exit 1
fi

echo -n "      libobs headers: "
if [ -f /usr/include/obs/obs-module.h ]; then
    echo "Found at /usr/include/obs"
else
    echo "ERROR: /usr/include/obs/obs-module.h not found!"
    exit 1
fi

# 5. OpenGL / EGL Vendor
echo -n "[5/7] OpenGL / EGL Vendor: "
if command -v glxinfo >/dev/null 2>&1; then
    GL_VENDOR=$(glxinfo -B 2>/dev/null | grep "OpenGL vendor string" | cut -d: -f2 | xargs || true)
    GL_RENDERER=$(glxinfo -B 2>/dev/null | grep "OpenGL renderer string" | cut -d: -f2 | xargs || true)
    echo "$GL_VENDOR ($GL_RENDERER)"
else
    echo "glxinfo not found, checking via python/cuda"
fi

# 6. NVIDIA Video Effects SDK Libraries
echo "[6/7] NVIDIA Video Effects SDK (Maxine / RTX Video):"
SDK_PATHS=(
    "${LINRTX_SDK_PATH:-}"
    "$HOME/ComfyUI/.venv/lib/python3.12/site-packages/nvvfx/libs"
    "/usr/local/VideoFX/lib"
    "/opt/nvidia/libnvvfx"
)

FOUND_SDK=""
for p in "${SDK_PATHS[@]}"; do
    if [ -n "$p" ] && [ -d "$p" ] && [ -f "$p/libVideoFX.so" ] && [ -f "$p/libnvVFXVideoSuperRes.so" ]; then
        FOUND_SDK="$p"
        break
    fi
done

if [ -n "$FOUND_SDK" ]; then
    echo "      Found valid VFX SDK directory at: $FOUND_SDK"
    echo "      Libraries detected:"
    for lib in libVideoFX.so libNVCVImage.so libnvVFXVideoSuperRes.so libnvVFXTrueHDR.so libnvidia-ngx-vsr.so.1.8.3 libnvidia-ngx-truehdr.so.1.2.3; do
        if [ -f "$FOUND_SDK/$lib" ]; then
            echo "        - $lib: OK"
        else
            echo "        - $lib: MISSING"
        fi
    done
else
    echo "      WARNING: VFX SDK libraries not found in standard paths."
    echo "      (Install via 'pip install nvidia-vfx --index-url https://pypi.nvidia.com' or set LINRTX_SDK_PATH)"
fi

# 7. Build Tools
echo "[7/7] Build Toolchain:"
for tool in gcc g++ cmake ninja make pkg-config compute-sanitizer; do
    echo -n "      $tool: "
    if command -v "$tool" >/dev/null 2>&1; then
        echo "OK ($(command -v "$tool"))"
    else
        echo "MISSING"
    fi
done

echo "=========================================================="
echo " Environment Verification Finished"
echo "=========================================================="
