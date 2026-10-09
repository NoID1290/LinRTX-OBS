# LinRTX-OBS Research & Technical Findings

## 1. Environment Verification Output

Running `scripts/check-env.sh` on the target host produces:

```text
==========================================================
 LinRTX-OBS Environment Verification
==========================================================
[1/7] Distro & Kernel: CachyOS (Kernel 7.2.9-1-cachyos)
[2/7] NVIDIA Driver & GPU: NVIDIA GeForce RTX 4070 Ti (Driver 615.71.09, Compute Capability sm_8.9)
[3/7] CUDA Toolkit: Found nvcc (13.4 at /opt/cuda/bin/nvcc)
[4/7] OBS Studio: OBS Studio - 32.2.2 (/usr/bin/obs)
      libobs headers: Found at /usr/include/obs
[5/7] OpenGL / EGL Vendor: NVIDIA Corporation (NVIDIA GeForce RTX 4070 Ti/PCIe/SSE2)
[6/7] NVIDIA Video Effects SDK (Maxine / RTX Video):
      Found valid VFX SDK directory at: $HOME/ComfyUI/.venv/lib/python3.12/site-packages/nvvfx/libs
      Libraries detected:
        - libVideoFX.so: OK
        - libNVCVImage.so: OK
        - libnvVFXVideoSuperRes.so: OK
        - libnvVFXTrueHDR.so: OK
        - libnvidia-ngx-vsr.so.1.8.3: OK
        - libnvidia-ngx-truehdr.so.1.2.3: OK
[7/7] Build Toolchain:
      gcc: OK (/usr/bin/gcc)
      g++: OK (/usr/bin/g++)
      cmake: MISSING
      ninja: MISSING
      make: OK (/usr/bin/make)
      pkg-config: OK (/usr/bin/pkg-config)
      compute-sanitizer: OK (/opt/cuda/bin/compute-sanitizer)
==========================================================
 Environment Verification Finished
==========================================================
```

---

## 2. NVIDIA SDK Availability on Linux

### 2.1 Distribution and Packaging
Official Linux binaries for Video Super Resolution (VSR) and TrueHDR are distributed by NVIDIA through two channels:
1. **NVIDIA PyPI Wheel (`nvidia-vfx`)**:
   - Host repository: `https://pypi.nvidia.com`
   - Command: `python -m pip install nvidia-vfx --index-url https://pypi.nvidia.com`
   - Contains native x86_64 ELF shared libraries:
     - `libVideoFX.so`, `libVideoFXLocal.so` (Core VFX dispatch)
     - `libNVCVImage.so` (Image format, memory management, and transfer utilities)
     - `libnvVFXVideoSuperRes.so`, `libnvidia-ngx-vsr.so.1.8.3` (Video Super Resolution effect + NGX model weights)
     - `libnvVFXTrueHDR.so`, `libnvidia-ngx-truehdr.so.1.2.3` (TrueHDR SDR-to-HDR10 effect + NGX model weights)
     - `libcudnn.so.9`, `libnvngxruntime.so`, and CUDA NPP 12 libraries (`libnppc`, `libnppif`, `libnppig`, etc.)
   - Notice that the neural network models for VSR and TrueHDR are embedded directly inside `libnvidia-ngx-vsr.so` (57 MB) and `libnvidia-ngx-truehdr.so` (3.7 MB), eliminating separate `.onnx` or `.engine` file dependencies.
2. **NVIDIA Maxine VFX SDK / NGC**:
   - Packaged as tarballs for Enterprise/Developer accounts.
   - Headers and API definitions match the public NVIDIA Maxine VFX SDK interfaces.

### 2.2 License and EULA
NVIDIA Video Effects SDK binaries are proprietary and cannot be redistributed inside open-source git repositories.
- `LinRTX-OBS` will strictly dynamically load (`dlopen`) these libraries at runtime.
- Search path priority:
  1. `LINRTX_SDK_PATH` environment variable.
  2. Plugin configuration string set in OBS filter properties.
  3. Python virtual environments (e.g. `~/.local/lib/python3.*/site-packages/nvvfx/libs` or active venvs).
  4. Standard system directories (`/usr/local/VideoFX/lib`, `/opt/nvidia/vfx`, `/usr/lib64`).
- If missing, the plugin fails gracefully, leaves OBS running, displays clear diagnostic messages in filter properties and logs, and passes original video through untouched.

---

## 3. C API & SDK Signatures

### 3.1 Core Types & Enums
From `nvVideoEffects.h` and `nvCVImage.h`:
```c
typedef enum NvCV_Status {
    NVCV_SUCCESS = 0,
    NVCV_ERR_GENERAL = -1,
    NVCV_ERR_UNIMPLEMENTED = -2,
    NVCV_ERR_MEMORY = -3,
    NVCV_ERR_EFFECT = -4,
    NVCV_ERR_SELECTOR = -5,
    NVCV_ERR_BUFFER = -6,
    NVCV_ERR_PARAMETER = -7,
    NVCV_ERR_MISMATCH = -8,
    NVCV_ERR_PIXELFORMAT = -9,
    NVCV_ERR_MODEL = -10,
    NVCV_ERR_INITIALIZATION = -12,
    NVCV_ERR_CUDA = -1099,
} NvCV_Status;

typedef enum NvCVImage_PixelFormat {
    NVCV_FORMAT_UNKNOWN = 0,
    NVCV_Y = 1,
    NVCV_A = 2,
    NVCV_YA = 3,
    NVCV_RGB = 4,
    NVCV_BGR = 5,
    NVCV_RGBA = 6,
    NVCV_BGRA = 7,
    NVCV_ARGB = 8,
    NVCV_ABGR = 9,
    NVCV_YUV420 = 10,
    NVCV_YUV422 = 11,
    NVCV_YUV444 = 12,
    NVCV_RGB10A2 = 13,
} NvCVImage_PixelFormat;

typedef enum NvCVImage_ComponentType {
    NVCV_TYPE_UNKNOWN = 0,
    NVCV_U8 = 1,
    NVCV_U16 = 2,
    NVCV_S16 = 3,
    NVCV_F16 = 4,
    NVCV_U32 = 5,
    NVCV_S32 = 6,
    NVCV_F32 = 7,
    NVCV_P32 = 11, // Packed 32-bit (e.g. RGB10A2)
} NvCVImage_ComponentType;

#define NVCV_CHUNKY 0
#define NVCV_PLANAR 1
#define NVCV_CUDA   1

typedef struct NvCVImage {
    unsigned int width;
    unsigned int height;
    signed int pitch;
    NvCVImage_PixelFormat pixelFormat;
    NvCVImage_ComponentType componentType;
    unsigned char pixelBytes;
    unsigned char componentBytes;
    unsigned char numComponents;
    unsigned char planar;
    unsigned char gpuMem;
    unsigned char colorspace;
    unsigned char reserved[2];
    void *pixels;
    void *deletePtr;
    void (*deleteProc)(void *p);
    unsigned long long bufferBytes;
} NvCVImage;

typedef void *NvVFX_Handle;
typedef struct CUstream_st *CUstream;
```

### 3.2 Effects & Parameters
1. **Video Super Resolution (`NVVFX_FX_VIDEO_SUPER_RES = "VideoSuperRes"`)**:
   - `SrcImage0`: `(RGBA | BGRA) + U8` chunky, in `NVCV_CUDA` memory.
   - `DstImage0`: Same format as input, dimensions = target output width/height.
   - `ImageEncodingMode`: `0` for 8-bit RGBA/BGRA, `1` for 10-bit RGB10A2.
   - `QualityLevel`:
     - `0` = Bicubic
     - `1` = Low (Speed optimized)
     - `2` = Medium (Balanced)
     - `3` = High (Default)
     - `4` = Ultra (Maximum quality)
     - `8..11` = Denoise (Same resolution)
     - `12..15` = Deblur (Same resolution)
     - `16..19` = High-bitrate (Clean source)
   - `Strength`: `[0.0, 1.0]` float.
   - `CudaStream`: CUDA stream handle.

2. **TrueHDR (`NVVFX_FX_TRUE_HDR = "TrueHDR"`)**:
   - `SrcImage0`: `RGBA + U8` chunky (4 bytes/pixel), in `NVCV_CUDA` memory.
   - `DstImage0`: `RGB10A2 + P32` chunky (packed 32-bit uint32 per pixel: 10 bits R, 10 bits G, 10 bits B, 2 bits A), in `NVCV_CUDA` memory. Output is HDR10 PQ (SMPTE ST.2084) BT.2020.
   - Parameters:
     - `Contrast`: `0..200`, default `100`
     - `Saturation`: `0..200`, default `100`
     - `MiddleGray`: `10..100`, default `50`
     - `Luminance`: Target HDR display peak in nits, `400..2000`, default `650`
     - `DebandingOff`: `0` (deband on) or `1` (deband off), default `0`

3. **Video Frame Generation (`NVVFX_FX_VIDEO_FRAME_GENERATION = "VideoFrameGeneration"`)**:
   - `InputWidth`, `InputHeight`: Input frame dimensions in pixels.
   - `SrcImage0`: Previous frame: `(RGBA | BGRA) + U8` or `RGB10A2 + P32` chunky, in `NVCV_CUDA` memory.
   - `SrcImage1`: Current frame: Same encoding and dimensions as `SrcImage0`, in `NVCV_CUDA` memory.
   - `DstImage0`: Interpolated output frame: Same encoding and dimensions as inputs, in `NVCV_CUDA` memory.
   - `Mode`:
     - `0` = Low (Speed optimized, ~4.6 ms on RTX 4070 Ti)
     - `1` = Medium (Balanced, ~1.9 ms / 520 FPS on RTX 4070 Ti)
     - `2` = High (Maximum quality, ~18.9 ms on RTX 4070 Ti)
   - `FrameMultiplier`: Integer multiplier in `[2, 8]`. Set to `0` when using explicit `Timestep`.
   - `FrameIndex`: Sub-frame index in `[1, FrameMultiplier - 1]` when `FrameMultiplier != 0`.
   - `Timestep`: Continuous temporal position in `(0.0, 1.0)` when `FrameMultiplier == 0`.
   - `AutomaticShotChangeDetectionEnabled`: `1` (enabled by default) or `0`.
   - `ShotChange`: Manual shot change flag (`1` or `0`).
   - `ImageEncodingMode`: `0` for 8-bit RGBA, `1` for 10-bit RGB10A2.
   - `CudaStream`: CUDA stream handle.

---

## 4. CUDA–OpenGL Interop on Linux (Wayland / EGL)

On Linux Wayland with NVIDIA drivers:
1. OBS initializes an EGL display and OpenGL context (`NVIDIA Corporation 4.6.0 NVIDIA 615.71.09`).
2. Texture sharing between OpenGL and CUDA uses the standard CUDA Graphics Interop API (`cuda_gl_interop.h`):
   ```c
   cudaGraphicsResource_t cuda_res;
   cudaGraphicsGLRegisterImage(&cuda_res, gl_tex_id, GL_TEXTURE_2D, cudaGraphicsRegisterFlagsNone);
   ```
3. In `video_render`:
   ```c
   cudaGraphicsMapResources(1, &cuda_res, stream);
   cudaArray_t mapped_array;
   cudaGraphicsSubResourceGetMappedArray(&mapped_array, cuda_res, 0, 0);
   ```
4. Copying:
   - From GL input texture array into linear CUDA buffer:
     `cudaMemcpy2DFromArrayAsync(input_buffer, pitch, mapped_array, 0, 0, width * bytes_per_pixel, height, cudaMemcpyDeviceToDevice, stream);`
   - From output linear CUDA buffer into GL output texture array:
     `cudaMemcpy2DToArrayAsync(dst_mapped_array, 0, 0, output_buffer, pitch, width * bytes_per_pixel, height, cudaMemcpyDeviceToDevice, stream);`
   - `cudaGraphicsUnmapResources(1, &cuda_res, stream);`
5. No CPU readback (`glReadPixels` or `gs_stage_texture`) is involved. All operations remain entirely in VRAM.

---

## 5. OBS Studio 32.x Color Architecture & HDR Pipeline

### 5.1 Working Color Spaces
- **SDR Canvas**: `GS_CS_SRGB` (sRGB transfer, Rec. 709 primaries).
- **HDR Canvas**: `GS_CS_709_SCRGB` or `GS_CS_709_EXTENDED`.
  - Linear Rec. 709 primaries.
  - scRGB uses `1.0 = 80 nits`. The SDR paper white level is obtained via `obs_get_video_sdr_white_level()` (default typically 100 to 300 nits, e.g. 200 nits).

### 5.2 TrueHDR to OBS scRGB Conversion
TrueHDR outputs PQ-encoded BT.2020 pixels in `RGB10A2` format.
OBS renders on an scRGB canvas with linear Rec. 709 primaries.
The transformation in our custom shader (`linrtx_hdr.effect`):
1. **Sample PQ BT.2020**: Unpack `RGB10A2` normalized components $(R_{pq}, G_{pq}, B_{pq}) \in [0, 1]$.
2. **PQ EOTF to Absolute Linear Luminance ($cd/m^2$ or nits)**:
   $$c = u^{1 / 78.84375}$$
   $$L = 10000 \times \left(\frac{\max(c - 0.8359375, 0)}{18.8515625 - 18.6875 \cdot c}\right)^{1 / 0.1593017578}$$
3. **Gamut Conversion (BT.2020 $\to$ BT.709)**:
   Using standard ITU-R BT.2020 to BT.709 3x3 matrix:
   $$\begin{bmatrix} R_{709} \\ G_{709} \\ B_{709} \end{bmatrix} = \begin{bmatrix} 1.660491 & -0.587641 & -0.072850 \\ -0.124550 & 1.132900 & -0.008349 \\ -0.018151 & -0.100579 & 1.118730 \end{bmatrix} \begin{bmatrix} R_{2020} \\ G_{2020} \\ B_{2020} \end{bmatrix}$$
4. **Scale to scRGB ($1.0 = 80\text{ nits}$)**:
   $$v_{scRGB} = \frac{v_{709}}{80.0}$$
5. **SDR Canvas Fallback / Tonemapping**:
   When canvas is SDR (`GS_CS_SRGB`), apply Reinhard / EETF tonemapping and convert linear to nonlinear sRGB gamma. No blown-out highlights or clipping.

---

## 6. Prior Art Citations
- `obs-studio/plugins/nv-filters`: Reference implementation for dynamic loader (`nvvfx-load.h`) and `obs_source_info` filter callbacks.
- `comfyui_nvidia_rtx_nodes`: Verified working Linux implementation utilizing the PyPI `nvidia-vfx` SDK distribution.
- `libobs/data/color.effect`: Reference mathematical definitions for EOTF, inverse EOTF, color space conversion matrices, and EETF tonemapping.
