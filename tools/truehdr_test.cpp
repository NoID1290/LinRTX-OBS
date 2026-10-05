#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "include/stb_image_write.h"

#include "../src/nvvfx-loader.h"
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>

static void create_sdr_landscape_pattern(int width, int height, std::vector<uint8_t> &rgba) {
    rgba.resize(width * height * 4);
    for (int y = 0; y < height; ++y) {
        float ny = static_cast<float>(y) / height;
        for (int x = 0; x < width; ++x) {
            float nx = static_cast<float>(x) / width;
            int idx = (y * width + x) * 4;

            // Gradient sky with a bright sun disk + horizon
            float sun_dx = nx - 0.5f;
            float sun_dy = ny - 0.3f;
            float sun_dist = std::sqrt(sun_dx * sun_dx + sun_dy * sun_dy * 4.0f);

            float r, g, b;
            if (ny < 0.6f) { // Sky
                float sky_t = ny / 0.6f;
                r = 0.2f + 0.6f * sky_t;
                g = 0.4f + 0.5f * sky_t;
                b = 0.8f + 0.2f * sky_t;
                if (sun_dist < 0.12f) { // Sun
                    float glow = 1.0f - (sun_dist / 0.12f);
                    r = std::min(1.0f, r + glow * 1.5f);
                    g = std::min(1.0f, g + glow * 1.5f);
                    b = std::min(1.0f, b + glow * 1.2f);
                }
            } else { // Ground
                float ground_t = (ny - 0.6f) / 0.4f;
                r = 0.15f * (1.0f - ground_t);
                g = 0.45f * (1.0f - ground_t);
                b = 0.20f * (1.0f - ground_t);
            }

            rgba[idx + 0] = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
            rgba[idx + 1] = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
            rgba[idx + 2] = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
            rgba[idx + 3] = 255;
        }
    }
}

// ST.2084 (PQ) EOTF: normalized PQ [0..1] -> Linear Luminance in Nits [0..10000]
static inline float pq_to_nits(float v) {
    if (v <= 0.0f) return 0.0f;
    float c = std::pow(v, 1.0f / 78.84375f);
    float num = std::max(c - 0.8359375f, 0.0f);
    float den = 18.8515625f - 18.6875f * c;
    if (den <= 0.0f) return 10000.0f;
    return 10000.0f * std::pow(num / den, 1.0f / 0.1593017578f);
}

// Convert Rec.2020 linear to Rec.709 linear
static inline void rec2020_to_rec709(float r2020, float g2020, float b2020, float &r709, float &g709, float &b709) {
    r709 = 1.6604910f * r2020 - 0.5876411f * g2020 - 0.0728499f * b2020;
    g709 = -0.1245505f * r2020 + 1.1328999f * g2020 - 0.0083494f * b2020;
    b709 = -0.0181508f * r2020 - 0.1005789f * g2020 + 1.1187297f * b2020;
}

int main(int argc, char **argv) {
    std::cout << "=== LinRTX Standalone TrueHDR Test ===" << std::endl;

    std::string input_path = (argc > 1) ? argv[1] : "";
    int luminance = (argc > 2) ? std::stoi(argv[2]) : 1000;
    int contrast = (argc > 3) ? std::stoi(argv[3]) : 100;
    int saturation = (argc > 4) ? std::stoi(argv[4]) : 100;
    std::string out_prefix = (argc > 5) ? argv[5] : "tools/output_truehdr";

    int in_w = 0, in_h = 0, channels = 0;
    std::vector<uint8_t> host_in;

    if (!input_path.empty() && input_path != "--pattern") {
        uint8_t *data = stbi_load(input_path.c_str(), &in_w, &in_h, &channels, 4);
        if (!data) {
            std::cerr << "Failed to load input image: " << input_path << ", generating test pattern instead." << std::endl;
            in_w = 960;
            in_h = 540;
            create_sdr_landscape_pattern(in_w, in_h, host_in);
        } else {
            host_in.assign(data, data + in_w * in_h * 4);
            stbi_image_free(data);
        }
    } else {
        std::cout << "No input image supplied; generating 960x540 test pattern." << std::endl;
        in_w = 960;
        in_h = 540;
        create_sdr_landscape_pattern(in_w, in_h, host_in);
    }

    std::cout << "Input: " << in_w << "x" << in_h << std::endl;
    std::cout << "TrueHDR Settings: Luminance=" << luminance << " nits, Contrast=" << contrast << ", Saturation=" << saturation << std::endl;

    if (!linrtx::load_sdk()) {
        std::cerr << "Error loading VFX SDK: " << linrtx::get_sdk_error() << std::endl;
        return 1;
    }
    std::cout << "Loaded VFX SDK from: " << linrtx::get_sdk_path() << std::endl;

    const auto &api = linrtx::vfx();

    CUstream stream = nullptr;
    NvCV_Status err = api.CudaStreamCreate(&stream);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to create CUDA stream: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    // Allocate input (RGBA U8) and output (RGB10A2 P32) NvCVImage
    NvCVImage src_img = {};
    NvCVImage dst_img = {};

    err = api.ImageAlloc(&src_img, in_w, in_h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to allocate input NvCVImage: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    // Note: TrueHDR requires RGB10A2 (fmt 13) + P32 (componentType 11)
    err = api.ImageAlloc(&dst_img, in_w, in_h, NVCV_RGB10A2, NVCV_P32, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to allocate output NvCVImage (RGB10A2): " << linrtx::status_to_string(err) << std::endl;
        api.ImageDealloc(&src_img);
        return 1;
    }

    // Upload input to GPU
    cudaError_t cu_err = cudaMemcpy2DAsync(
        src_img.pixels, src_img.pitch,
        host_in.data(), in_w * 4,
        in_w * 4, in_h,
        cudaMemcpyHostToDevice, reinterpret_cast<cudaStream_t>(stream)
    );
    if (cu_err != cudaSuccess) {
        std::cerr << "cudaMemcpy2DAsync failed: " << cudaGetErrorString(cu_err) << std::endl;
        return 1;
    }

    // Create TrueHDR effect
    NvVFX_Handle handle = nullptr;
    err = api.CreateEffect(NVVFX_FX_TRUE_HDR, &handle);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to create TrueHDR effect: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    api.SetCudaStream(handle, NVVFX_PARAM_CUDA_STREAM, stream);
    api.SetU32(handle, NVVFX_PARAM_LUMINANCE, luminance);
    api.SetU32(handle, NVVFX_PARAM_CONTRAST, contrast);
    api.SetU32(handle, NVVFX_PARAM_SATURATION, saturation);
    api.SetU32(handle, NVVFX_PARAM_MIDDLE_GRAY, 50);
    api.SetU32(handle, NVVFX_PARAM_DEBANDING_OFF, 0);

    api.SetImage(handle, NVVFX_PARAM_SRC_IMAGE_0, &src_img);
    api.SetImage(handle, NVVFX_PARAM_DST_IMAGE_0, &dst_img);

    std::cout << "Loading TrueHDR effect..." << std::endl;
    err = api.Load(handle);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to Load TrueHDR effect: " << linrtx::status_to_string(err) << std::endl;
        api.DestroyEffect(handle);
        return 1;
    }
    std::cout << "TrueHDR loaded. Running warm-up..." << std::endl;

    // Warm-up run
    err = api.Run(handle, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Warm-up Run failed: " << linrtx::status_to_string(err) << std::endl;
        api.DestroyEffect(handle);
        return 1;
    }
    cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream));

    // Timed benchmark: 10 iterations
    const int iterations = 10;
    std::cout << "Running " << iterations << " benchmark iterations..." << std::endl;

    auto t_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        err = api.Run(handle, 0);
        if (err != NVCV_SUCCESS) {
            std::cerr << "Run iteration " << i << " failed: " << linrtx::status_to_string(err) << std::endl;
            break;
        }
    }
    cudaStreamSynchronize(reinterpret_cast<cudaStream_t>(stream));
    auto t_end = std::chrono::high_resolution_clock::now();

    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    double avg_ms = total_ms / iterations;
    double fps = 1000.0 / avg_ms;

    std::cout << "--------------------------------------------------" << std::endl;
    std::cout << "Benchmark results:" << std::endl;
    std::cout << "  Dimensions:  " << in_w << "x" << in_h << std::endl;
    std::cout << "  Avg latency: " << avg_ms << " ms/frame" << std::endl;
    std::cout << "  Throughput:  " << fps << " FPS" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    // Read back output RGB10A2 packed words
    std::vector<uint32_t> host_out(in_w * in_h);
    cu_err = cudaMemcpy2D(
        host_out.data(), in_w * sizeof(uint32_t),
        dst_img.pixels, dst_img.pitch,
        in_w * sizeof(uint32_t), in_h,
        cudaMemcpyDeviceToHost
    );
    if (cu_err != cudaSuccess) {
        std::cerr << "cudaMemcpy2D back to host failed: " << cudaGetErrorString(cu_err) << std::endl;
        return 1;
    }

    // Process output for:
    // 1. EXR linear RGB (3 channels float)
    // 2. SDR preview PNG (tonemapped Reinhard + sRGB gamma)
    std::vector<float> exr_r(in_w * in_h);
    std::vector<float> exr_g(in_w * in_h);
    std::vector<float> exr_b(in_w * in_h);
    std::vector<uint8_t> sdr_preview(in_w * in_h * 3);

    float max_nits_observed = 0.0f;

    for (int y = 0; y < in_h; ++y) {
        for (int x = 0; x < in_w; ++x) {
            int idx = y * in_w + x;
            uint32_t pixel = host_out[idx];

            // Unpack 10-bit components
            uint32_t r_u10 = (pixel >> 0) & 0x3FF;
            uint32_t g_u10 = (pixel >> 10) & 0x3FF;
            uint32_t b_u10 = (pixel >> 20) & 0x3FF;

            // Normalized PQ [0, 1]
            float r_pq = r_u10 / 1023.0f;
            float g_pq = g_u10 / 1023.0f;
            float b_pq = b_u10 / 1023.0f;

            // Convert PQ to linear nits [0, 10000]
            float r_nits = pq_to_nits(r_pq);
            float g_nits = pq_to_nits(g_pq);
            float b_nits = pq_to_nits(b_pq);

            max_nits_observed = std::max({max_nits_observed, r_nits, g_nits, b_nits});

            // Convert BT.2020 linear to BT.709 linear
            float r709_nits, g709_nits, b709_nits;
            rec2020_to_rec709(r_nits, g_nits, b_nits, r709_nits, g709_nits, b709_nits);
            r709_nits = std::max(0.0f, r709_nits);
            g709_nits = std::max(0.0f, g709_nits);
            b709_nits = std::max(0.0f, b709_nits);

            // Normalized linear scene radiance (relative to 100 nits SDR reference)
            exr_r[idx] = r709_nits / 100.0f;
            exr_g[idx] = g709_nits / 100.0f;
            exr_b[idx] = b709_nits / 100.0f;

            // SDR Tonemapped preview: Reinhard tone reproduction -> sRGB gamma
            float norm_r = r709_nits / 100.0f;
            float norm_g = g709_nits / 100.0f;
            float norm_b = b709_nits / 100.0f;

            float tm_r = norm_r / (norm_r + 1.0f);
            float tm_g = norm_g / (norm_g + 1.0f);
            float tm_b = norm_b / (norm_b + 1.0f);

            // Gamma 2.2 approximation
            float srgb_r = std::pow(tm_r, 1.0f / 2.2f);
            float srgb_g = std::pow(tm_g, 1.0f / 2.2f);
            float srgb_b = std::pow(tm_b, 1.0f / 2.2f);

            sdr_preview[idx * 3 + 0] = static_cast<uint8_t>(std::clamp(srgb_r * 255.0f, 0.0f, 255.0f));
            sdr_preview[idx * 3 + 1] = static_cast<uint8_t>(std::clamp(srgb_g * 255.0f, 0.0f, 255.0f));
            sdr_preview[idx * 3 + 2] = static_cast<uint8_t>(std::clamp(srgb_b * 255.0f, 0.0f, 255.0f));
        }
    }

    std::cout << "Peak luminance observed in frame: " << max_nits_observed << " nits (target " << luminance << " nits)" << std::endl;

    // Save Radiance HDR (.hdr) linear floating-point image
    std::string hdr_path = out_prefix + ".hdr";
    std::vector<float> hdr_interleaved(in_w * in_h * 3);
    for (int i = 0; i < in_w * in_h; ++i) {
        hdr_interleaved[i * 3 + 0] = exr_r[i];
        hdr_interleaved[i * 3 + 1] = exr_g[i];
        hdr_interleaved[i * 3 + 2] = exr_b[i];
    }
    if (stbi_write_hdr(hdr_path.c_str(), in_w, in_h, 3, hdr_interleaved.data())) {
        std::cout << "Saved float HDR file to: " << hdr_path << std::endl;
    } else {
        std::cerr << "Failed to save HDR file to: " << hdr_path << std::endl;
    }

    // Save SDR Tonemapped Preview PNG
    std::string sdr_path = out_prefix + "_sdr_preview.png";
    if (stbi_write_png(sdr_path.c_str(), in_w, in_h, 3, sdr_preview.data(), in_w * 3)) {
        std::cout << "Saved SDR Tonemapped Preview to: " << sdr_path << std::endl;
    } else {
        std::cerr << "Failed to save SDR preview to: " << sdr_path << std::endl;
    }

    // Cleanup
    api.DestroyEffect(handle);
    api.ImageDealloc(&src_img);
    api.ImageDealloc(&dst_img);
    api.CudaStreamDestroy(stream);
    linrtx::unload_sdk();

    std::cout << "=== TrueHDR Test PASSED ===" << std::endl;
    return 0;
}
