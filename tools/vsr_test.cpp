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

static void create_test_pattern(int width, int height, std::vector<uint8_t> &rgba) {
    rgba.resize(width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 4;
            // Radial circles + sharp checkerboard
            float cx = x - width / 2.0f;
            float cy = y - height / 2.0f;
            float dist = std::sqrt(cx * cx + cy * cy);
            bool ring = (static_cast<int>(dist / 16.0f) % 2) == 0;
            bool checker = ((x / 16) ^ (y / 16)) & 1;

            uint8_t r = ring ? 220 : 40;
            uint8_t g = checker ? 200 : 60;
            uint8_t b = static_cast<uint8_t>((x * 255) / width);
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = 255;
        }
    }
}

int main(int argc, char **argv) {
    std::cout << "=== LinRTX Standalone VSR Test ===" << std::endl;

    std::string input_path = (argc > 1) ? argv[1] : "";
    float scale = (argc > 2) ? std::stof(argv[2]) : 2.0f;
    int quality = (argc > 3) ? std::stoi(argv[3]) : 4; // 4 = ULTRA
    std::string output_path = (argc > 4) ? argv[4] : "tools/output_vsr.png";

    int in_w = 0, in_h = 0, channels = 0;
    std::vector<uint8_t> host_in;

    if (!input_path.empty() && input_path != "--pattern") {
        uint8_t *data = stbi_load(input_path.c_str(), &in_w, &in_h, &channels, 4);
        if (!data) {
            std::cerr << "Failed to load input image: " << input_path << ", generating test pattern instead." << std::endl;
            in_w = 960;
            in_h = 540;
            create_test_pattern(in_w, in_h, host_in);
        } else {
            host_in.assign(data, data + in_w * in_h * 4);
            stbi_image_free(data);
        }
    } else {
        std::cout << "No input image supplied; generating 960x540 test pattern." << std::endl;
        in_w = 960;
        in_h = 540;
        create_test_pattern(in_w, in_h, host_in);
    }

    int out_w = static_cast<int>(in_w * scale);
    int out_h = static_cast<int>(in_h * scale);
    // Align to 4 or 8
    out_w = (out_w + 3) & ~3;
    out_h = (out_h + 3) & ~3;

    std::cout << "Input:  " << in_w << "x" << in_h << std::endl;
    std::cout << "Output: " << out_w << "x" << out_h << " (scale " << scale << "x, Quality=" << quality << ")" << std::endl;

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

    // Allocate input and output NvCVImage
    NvCVImage src_img = {};
    NvCVImage dst_img = {};

    err = api.ImageAlloc(&src_img, in_w, in_h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to allocate input NvCVImage: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    err = api.ImageAlloc(&dst_img, out_w, out_h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to allocate output NvCVImage: " << linrtx::status_to_string(err) << std::endl;
        api.ImageDealloc(&src_img);
        return 1;
    }

    // Copy host input to src_img GPU memory
    cudaError_t cu_err = cudaMemcpy2DAsync(
        src_img.pixels, src_img.pitch,
        host_in.data(), in_w * 4,
        in_w * 4, in_h,
        cudaMemcpyHostToDevice, reinterpret_cast<cudaStream_t>(stream)
    );
    if (cu_err != cudaSuccess) {
        std::cerr << "cudaMemcpy2DAsync to device failed: " << cudaGetErrorString(cu_err) << std::endl;
        return 1;
    }

    // Create VideoSuperRes effect
    NvVFX_Handle handle = nullptr;
    err = api.CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &handle);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to create VideoSuperRes effect: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    api.SetCudaStream(handle, NVVFX_PARAM_CUDA_STREAM, stream);
    api.SetU32(handle, NVVFX_PARAM_IMAGE_ENCODING, 0); // 8-bit RGBA
    api.SetU32(handle, NVVFX_PARAM_QUALITY_LEVEL, quality);
    api.SetImage(handle, NVVFX_PARAM_SRC_IMAGE_0, &src_img);
    api.SetImage(handle, NVVFX_PARAM_DST_IMAGE_0, &dst_img);

    std::cout << "Loading VideoSuperRes effect..." << std::endl;
    err = api.Load(handle);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to Load effect: " << linrtx::status_to_string(err) << std::endl;
        api.DestroyEffect(handle);
        return 1;
    }
    std::cout << "Effect loaded successfully. Running warm-up..." << std::endl;

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
    std::cout << "  Input:       " << in_w << "x" << in_h << std::endl;
    std::cout << "  Output:      " << out_w << "x" << out_h << std::endl;
    std::cout << "  Avg latency: " << avg_ms << " ms/frame" << std::endl;
    std::cout << "  Throughput:  " << fps << " FPS" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    // Read back output from GPU
    std::vector<uint8_t> host_out(out_w * out_h * 4);
    cu_err = cudaMemcpy2D(
        host_out.data(), out_w * 4,
        dst_img.pixels, dst_img.pitch,
        out_w * 4, out_h,
        cudaMemcpyDeviceToHost
    );
    if (cu_err != cudaSuccess) {
        std::cerr << "cudaMemcpy2D back to host failed: " << cudaGetErrorString(cu_err) << std::endl;
        return 1;
    }

    if (stbi_write_png(output_path.c_str(), out_w, out_h, 4, host_out.data(), out_w * 4)) {
        std::cout << "Saved upscaled output to: " << output_path << std::endl;
    } else {
        std::cerr << "Failed to write output image to " << output_path << std::endl;
    }

    // Cleanup
    api.DestroyEffect(handle);
    api.ImageDealloc(&src_img);
    api.ImageDealloc(&dst_img);
    api.CudaStreamDestroy(stream);
    linrtx::unload_sdk();

    std::cout << "=== VSR Test PASSED ===" << std::endl;
    return 0;
}
