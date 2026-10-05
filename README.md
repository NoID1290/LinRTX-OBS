# LinRTX-OBS: Native NVIDIA RTX Video Filters for OBS Studio on Linux

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2%2B-blue.svg)](LICENSE)
[![OBS Studio](https://img.shields.io/badge/OBS%20Studio-30.x%20--%2032.x-orange.svg)](https://obsproject.com/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20(Wayland%2FEGL%20%26%20X11)-brightgreen.svg)]()
[![CUDA](https://img.shields.io/badge/CUDA-12.x%20%2F%2013.x-green.svg)](https://developer.nvidia.com/cuda-toolkit)

**LinRTX-OBS** is a high-performance, native Linux OBS Studio plugin that brings NVIDIA RTX AI Video Enhancement technologies to Linux content creators and streamers:
- **RTX Video Super Resolution (VSR):** Deep learning AI video upscaling and artifact removal.
- **RTX Video HDR (TrueHDR):** Real-time inverse tone mapping converting SDR video sources to 10-bit HDR (Rec.2100 PQ / BT.2020).

Implemented with **zero-copy CUDA–OpenGL interop** via `cudaGraphicsGLRegisterImage`, running entirely on the GPU without per-frame CPU readbacks or bus transfers.

---

## Key Features

- **Zero-Copy GPU Pipeline:** OBS OpenGL textures are mapped directly into CUDA address space via hardware texture arrays. Video frames remain in VRAM throughout capture, inference, and rendering.
- **RTX Video Super Resolution:**
  - AI upscaling from any resolution (360p/720p/1080p/1440p) up to 4K.
  - Multi-scale modes: Custom scale multiplier (1.0x–4.0x) or standard target resolutions (1080p, 1440p, 4K).
  - 4 AI Quality Levels: Level 1 (Fast), Level 2 (Medium), Level 3 (High), Level 4 (Ultra Quality).
- **RTX Video HDR (TrueHDR):**
  - Converts SDR content (Rec.709 sRGB) to authentic high dynamic range video.
  - Tunable target peak luminance (400 to 2000 nits, default 1000 nits).
  - Dynamic controls for contrast, saturation, and middle-gray exposure.
  - Dual-mode color rendering: Full floating-point scRGB (`GS_CS_709_SCRGB`) for HDR canvases and Reinhard tone mapping for SDR canvases.
- **Filter Chaining:** Seamlessly chain VSR and TrueHDR together (e.g., upscale 720p SDR $\rightarrow$ 1440p SDR via VSR, followed by 1440p SDR $\rightarrow$ 1440p HDR via TrueHDR).
- **Graceful Error Handling:** If the NVIDIA VFX SDK is unavailable or incompatible, filters fall back to non-blocking pass-through mode without crashing OBS.
- **Dynamic Runtime Loader:** Detects and binds NVIDIA Video Effects SDK libraries dynamically from PyPI wheels, virtual environments (e.g., ComfyUI), or custom directories without requiring root installation.

---

## Hardware & Software Requirements

| Component | Minimum Requirement | Verified Test Platform |
| :--- | :--- | :--- |
| **GPU** | NVIDIA GeForce RTX 20/30/40/50 series | NVIDIA GeForce RTX 4070 Ti (sm_89, 12GB) |
| **Driver** | NVIDIA Linux Proprietary Driver $\ge$ 530.41 | NVIDIA 615.71.09 |
| **CUDA** | CUDA Toolkit $\ge$ 12.0 | CUDA 13.4 |
| **OBS Studio** | OBS Studio 30.0+ with OpenGL/EGL support | OBS Studio 32.2.2 (Wayland / EGL) |
| **Display Server** | Wayland or X11 | Wayland (KWin / Plasma 6.6) |
| **OS** | Any Linux distribution (Arch, CachyOS, Fedora, Ubuntu) | CachyOS (Linux Kernel 7.2.9) |

---

## Verified Performance Benchmarks

Measured on **NVIDIA GeForce RTX 4070 Ti (Ada Lovelace, sm_89)** with native OBS Studio 32.2.2:

### 1. RTX Video Super Resolution (VSR)
| Input $\rightarrow$ Output Resolution | Scale Factor | Quality Level | Latency (ms) | Throughput (FPS) | VRAM Delta |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **1080p $\rightarrow$ 4K (3840x2160)** | 2.0x | Level 4 (Ultra) | **8.91 ms** | **112.2 FPS** | +142 MB |
| **1080p $\rightarrow$ 4K (3840x2160)** | 2.0x | Level 3 (High) | **4.72 ms** | **211.9 FPS** | +128 MB |
| **1080p $\rightarrow$ 4K (3840x2160)** | 2.0x | Level 2 (Medium) | **3.56 ms** | **280.9 FPS** | +124 MB |
| **1080p $\rightarrow$ 4K (3840x2160)** | 2.0x | Level 1 (Fast) | **2.65 ms** | **377.4 FPS** | +120 MB |
| **720p $\rightarrow$ 1440p (2560x1440)** | 2.0x | Level 4 (Ultra) | **4.82 ms** | **207.5 FPS** | +86 MB |

### 2. RTX Video HDR (TrueHDR)
| Input Resolution | Peak Target | Latency (ms) | Throughput (FPS) | Measured Peak Luminance |
| :--- | :---: | :---: | :---: | :---: |
| **720p (1280x720)** | 1000 nits | **1.21 ms** | **826.4 FPS** | 998.9 nits |
| **1080p (1920x1080)** | 1000 nits | **1.77 ms** | **565.8 FPS** | 998.9 nits |
| **1440p (2560x1440)** | 1000 nits | **3.12 ms** | **320.5 FPS** | 999.1 nits |

### 3. Chained Pipeline (VSR 2.0x Ultra + TrueHDR 1000 nits)
| Pipeline Configuration | Latency (ms) | Observed OBS Frame Rate | VRAM Stability |
| :--- | :---: | :---: | :---: |
| **720p SDR $\rightarrow$ 1440p HDR10** | **12.65 ms** | **79.1 FPS** (Rock-solid 60 FPS target) | **$\pm 0$ MB leak across 60s soak test** |

---

## Installation Guide

### Option A: Arch Linux / CachyOS (PKGBUILD)

```bash
# Clone the repository
git clone https://github.com/NoID1290/LinRTX-OBS.git
cd LinRTX-OBS

# Build and install package
makepkg -si
```

### Option B: Quick User Install via Make

```bash
git clone https://github.com/NoID1290/LinRTX-OBS.git
cd LinRTX-OBS

# Build and install to ~/.config/obs-studio/plugins/linrtx-obs
make install-user
```

### Option C: CMake Build

```bash
git clone https://github.com/NoID1290/LinRTX-OBS.git
cd LinRTX-OBS

cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_VSR=ON \
    -DENABLE_TRUEHDR=ON

cmake --build build
cmake --build build --target install-user
```

---

## NVIDIA VFX SDK Setup

LinRTX-OBS dynamically loads NVIDIA's Video Effects SDK (`libVideoFX.so`, `libnvVFXVideoSuperRes.so`, `libnvVFXTrueHDR.so`). 

NVIDIA distributes the official Linux SDK packages through PyPI. You can install them into a Python virtual environment or system Python:

```bash
# Option 1: Install into your user environment
pip install nvidia-vfx --extra-index-url https://pypi.nvidia.com

# Option 2: Reuse an existing environment (e.g., ComfyUI)
# If you have ComfyUI installed, the plugin automatically discovers:
# ~/ComfyUI/.venv/lib/python3.12/site-packages/nvvfx/libs

# Option 3: Custom SDK Path
# Specify your SDK directory via an environment variable before launching OBS:
export LINRTX_SDK_PATH="/opt/nvidia/vfx/libs"
obs
```

---

## Usage Instructions

### Adding RTX Video Super Resolution (VSR)
1. Open OBS Studio.
2. Right-click any video source (Capture Card, Media Source, Window Capture) $\rightarrow$ **Filters**.
3. Under **Effect Filters**, click **+** and choose **NVIDIA RTX Video Super Resolution**.
4. Configure options:
   - **Scaling Mode:** Multiplier (1.0x to 4.0x) or Target Resolution (1080p, 1440p, 4K).
   - **Quality Preset:** Fast (1), Medium (2), High (3), Ultra (4).

### Adding RTX Video HDR (TrueHDR)
1. In OBS Studio, ensure your canvas or recording profile supports HDR:
   - **Settings** $\rightarrow$ **Advanced** $\rightarrow$ **Color Format:** `P010` (10-bit).
   - **Color Space:** `Rec. 2100 (PQ)` or `Rec. 2100 (HLG)`.
2. Right-click your SDR source $\rightarrow$ **Filters** $\rightarrow$ **+** $\rightarrow$ **NVIDIA RTX HDR (TrueHDR)**.
3. Configure HDR parameters:
   - **Peak Luminance:** Target peak brightness in nits (400–2000 nits, default 1000 nits).
   - **Contrast & Saturation:** Fine-tune dynamic range and color vibrancy.
   - **Middle Gray Reference:** Adjust mid-tone exposure curve.

### Chaining VSR + TrueHDR
Apply **NVIDIA RTX Video Super Resolution** first, followed by **NVIDIA RTX HDR (TrueHDR)** in the filter list. VSR upscales the SDR frame with neural super resolution, and TrueHDR transforms the upscaled frame into 10-bit HDR10.

---

## Architecture & Technical Details

```mermaid
flowchart LR
    subgraph OBS_GL["OBS Graphics Subsystem (OpenGL / EGL)"]
        SourceTex["Source Texture (SDR RGBA8)"]
        FBO["Intermediate TexRender"]
        OutTex["Output Texture (scRGB FP16)"]
    end

    subgraph CUDA_Interop["Zero-Copy CUDA-GL Interop"]
        MapIn["cudaGraphicsMapResources(arr_in)"]
        MapOut["cudaGraphicsMapResources(arr_out)"]
    end

    subgraph Maxine_SDK["NVIDIA Maxine VFX SDK"]
        VSR["VSR Inference Engine\n(libnvVFXVideoSuperRes.so)"]
        TrueHDR["TrueHDR Inference Engine\n(libnvVFXTrueHDR.so)"]
        Unpack["CUDA Kernel\nRGB10A2 -> RGBA16F"]
    end

    SourceTex --> FBO
    FBO --> MapIn
    MapIn --> VSR
    VSR --> TrueHDR
    TrueHDR --> Unpack
    Unpack --> MapOut
    MapOut --> OutTex
```

1. **OpenGL-CUDA Texture Registration:** Textures are created with `GS_RGBA` / `GS_RGBA16F` and registered via `cudaGraphicsGLRegisterImage` with `cudaGraphicsRegisterFlagsReadOnly` and `cudaGraphicsRegisterFlagsWriteDiscard`.
2. **CUDA Stream Synchronization:** All transfers and inferences run asynchronously on a dedicated CUDA stream (`CUstream`) to prevent blocking the OBS main render loop.
3. **Format Conversion Kernel:** TrueHDR outputs packed `R10G10B10A2` (32-bit integer). Our CUDA kernel unpacks this into IEEE 754 half-precision float (`half4` / `RGBA16F`) on the GPU before copying to the OBS texture.
4. **Shader Stage (`linrtx_hdr.effect`):** Computes PQ ST.2084 electro-optical transfer function (EOTF) decoding, BT.2020 to Rec.709 matrix transformation, and converts linear luminance to scRGB standard ($1.0 = 80\text{ nits}$).

---

## Troubleshooting

- **Filter Status shows "SDK: NOT FOUND":**
  Ensure NVIDIA Video Effects SDK libraries are installed. Run:
  `pip install nvidia-vfx --extra-index-url https://pypi.nvidia.com`
  or verify `echo $LINRTX_SDK_PATH`.
- **Black screen on filter toggle:**
  LinRTX-OBS incorporates non-blocking fallbacks. If CUDA context loss or VRAM exhaustion occurs, check OBS log files (`~/.config/obs-studio/logs/`).
- **HDR colors appear washed out in preview:**
  Ensure your OBS preview is configured for HDR display or your monitor supports HDR10. On SDR monitors, LinRTX-OBS includes an automatic Reinhard tonemapping preview.

---

## License

This project is licensed under the **GNU General Public License v2.0 or later** ([GPL-2.0-or-later](LICENSE)).

*NVIDIA, GeForce, RTX, and Maxine are trademarks or registered trademarks of NVIDIA Corporation. This project is an independent open-source contribution and is not affiliated with or endorsed by NVIDIA Corporation.*
