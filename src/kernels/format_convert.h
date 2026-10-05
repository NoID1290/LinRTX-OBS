#pragma once

#include <cstdint>
#include <cstddef>
#include <cuda_runtime.h>

namespace linrtx {

// Unpacks packed R10G10B10A2 uint32 pixels to FP16 RGBA (half4)
void launch_unpack_rgb10a2_to_rgba16f(
    const void *src_packed, size_t src_pitch,
    void *dst_half4, size_t dst_pitch,
    uint32_t width, uint32_t height,
    cudaStream_t stream
);

// Simple color inversion kernel for RGBA8 testing
void launch_invert_rgba8(
    void *data, size_t pitch,
    uint32_t width, uint32_t height,
    cudaStream_t stream
);

} // namespace linrtx
