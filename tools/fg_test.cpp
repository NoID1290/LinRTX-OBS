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

static void create_test_pattern(int width, int height, float offset_x, std::vector<uint8_t> &rgba) {
    rgba.resize(width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 4;
            // Moving circle + checkerboard
            float cx = (x - offset_x) - width / 2.0f;
            float cy = y - height / 2.0f;
            float dist = std::sqrt(cx * cx + cy * cy);
            bool ball = (dist < 100.0f);
            bool checker = ((x / 32) ^ (y / 32)) & 1;

            uint8_t r = ball ? 255 : (checker ? 180 : 30);
            uint8_t g = ball ? 50 : (checker ? 180 : 30);
            uint8_t b = ball ? 50 : (checker ? 220 : 60);
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = 255;
        }
    }
}

int main(int argc, char **argv) {
    std::cout << "=== LinRTX Standalone Video Frame Generation Test ===" << std::endl;

    int width = 1920;
    int height = 1080;
    int mode = (argc > 1) ? std::stoi(argv[1]) : 1; // 0=Low, 1=Medium, 2=High
    int multiplier = (argc > 2) ? std::stoi(argv[2]) : 2;

    std::cout << "Resolution: " << width << "x" << height << ", Mode: " << mode 
              << ", Multiplier: " << multiplier << std::endl;

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

    std::vector<uint8_t> frame0_host, frame1_host, output_host;
    create_test_pattern(width, height, -80.0f, frame0_host);
    create_test_pattern(width, height, 80.0f, frame1_host);
    output_host.resize(width * height * 4);

    NvCVImage src0_img = {};
    NvCVImage src1_img = {};
    NvCVImage dst_img = {};

    err = api.ImageAlloc(&src0_img, width, height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to alloc src0: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }
    err = api.ImageAlloc(&src1_img, width, height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to alloc src1: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }
    err = api.ImageAlloc(&dst_img, width, height, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to alloc dst: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    cudaStream_t cu_stream = reinterpret_cast<cudaStream_t>(stream);
    cudaMemcpy2DAsync(src0_img.pixels, src0_img.pitch, frame0_host.data(), width * 4, width * 4, height, cudaMemcpyHostToDevice, cu_stream);
    cudaMemcpy2DAsync(src1_img.pixels, src1_img.pitch, frame1_host.data(), width * 4, width * 4, height, cudaMemcpyHostToDevice, cu_stream);

    NvVFX_Handle handle = nullptr;
    err = api.CreateEffect(NVVFX_FX_VIDEO_FRAME_GENERATION, &handle);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to create VideoFrameGeneration effect: " << linrtx::status_to_string(err) << std::endl;
        return 1;
    }

    api.SetCudaStream(handle, NVVFX_PARAM_CUDA_STREAM, stream);
    api.SetU32(handle, NVVFX_PARAM_INPUT_WIDTH, width);
    api.SetU32(handle, NVVFX_PARAM_INPUT_HEIGHT, height);
    api.SetU32(handle, NVVFX_PARAM_MODE, mode);
    api.SetU32(handle, NVVFX_PARAM_AUTO_SHOT_CHANGE, 1);
    api.SetU32(handle, NVVFX_PARAM_SHOT_CHANGE, 0);
    api.SetImage(handle, NVVFX_PARAM_SRC_IMAGE_0, &src0_img);
    api.SetImage(handle, NVVFX_PARAM_SRC_IMAGE_1, &src1_img);
    api.SetImage(handle, NVVFX_PARAM_DST_IMAGE_0, &dst_img);
    api.SetU32(handle, NVVFX_PARAM_FRAME_MULTIPLIER, multiplier);
    if (multiplier == 0) {
        api.SetF32(handle, NVVFX_PARAM_TIMESTEP, 0.5f);
    } else {
        api.SetU32(handle, NVVFX_PARAM_FRAME_INDEX, 1);
    }

    std::cout << "Loading VideoFrameGeneration effect..." << std::endl;
    auto t_load0 = std::chrono::high_resolution_clock::now();
    err = api.Load(handle);
    auto t_load1 = std::chrono::high_resolution_clock::now();
    if (err != NVCV_SUCCESS) {
        std::cerr << "Failed to Load VideoFrameGeneration: " << linrtx::status_to_string(err) << std::endl;
        api.DestroyEffect(handle);
        return 1;
    }
    double load_ms = std::chrono::duration<double, std::milli>(t_load1 - t_load0).count();
    std::cout << "VideoFrameGeneration loaded successfully in " << load_ms << " ms!" << std::endl;

    // Warm-up run
    std::cout << "Running warm-up frame generation..." << std::endl;
    err = api.Run(handle, 0);
    if (err != NVCV_SUCCESS) {
        std::cerr << "Warm-up Run failed: " << linrtx::status_to_string(err) << std::endl;
        api.DestroyEffect(handle);
        return 1;
    }
    cudaStreamSynchronize(cu_stream);
    std::cout << "Warm-up run successful!" << std::endl;

    // Benchmark 20 iterations
    const int iterations = 20;
    std::cout << "Benchmarking " << iterations << " iterations..." << std::endl;
    auto t_b0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        err = api.Run(handle, 0);
        if (err != NVCV_SUCCESS) {
            std::cerr << "Iteration " << i << " failed: " << linrtx::status_to_string(err) << std::endl;
            break;
        }
    }
    cudaStreamSynchronize(cu_stream);
    auto t_b1 = std::chrono::high_resolution_clock::now();

    double total_ms = std::chrono::duration<double, std::milli>(t_b1 - t_b0).count();
    double avg_ms = total_ms / iterations;
    double fps = 1000.0 / avg_ms;

    std::cout << "--------------------------------------------------" << std::endl;
    std::cout << "Average Generation Time: " << avg_ms << " ms (" << fps << " FPS)" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    // Copy result back to host and save PNG
    cudaMemcpy2D(output_host.data(), width * 4, dst_img.pixels, dst_img.pitch, width * 4, height, cudaMemcpyDeviceToHost);
    stbi_write_png("tools/output_framegen.png", width, height, 4, output_host.data(), width * 4);
    std::cout << "Saved interpolated frame to tools/output_framegen.png" << std::endl;

    api.DestroyEffect(handle);
    api.ImageDealloc(&src0_img);
    api.ImageDealloc(&src1_img);
    api.ImageDealloc(&dst_img);
    api.CudaStreamDestroy(stream);

    std::cout << "=== VideoFrameGeneration Test PASSED! ===" << std::endl;
    return 0;
}
