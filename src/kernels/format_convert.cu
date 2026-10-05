#include "format_convert.h"
#include <cuda_fp16.h>

namespace linrtx {

__global__ void k_unpack_rgb10a2_to_rgba16f(
    const uint8_t *__restrict__ src_bytes, size_t src_pitch,
    uint8_t *__restrict__ dst_bytes, size_t dst_pitch,
    uint32_t width, uint32_t height
) {
    uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const uint32_t *row_src = reinterpret_cast<const uint32_t *>(src_bytes + y * src_pitch);
    __half *row_dst = reinterpret_cast<__half *>(dst_bytes + y * dst_pitch);

    uint32_t pixel = row_src[x];
    float r = ((pixel >> 0) & 0x3FF) * (1.0f / 1023.0f);
    float g = ((pixel >> 10) & 0x3FF) * (1.0f / 1023.0f);
    float b = ((pixel >> 20) & 0x3FF) * (1.0f / 1023.0f);
    float a = ((pixel >> 30) & 0x3) * (1.0f / 3.0f);

    size_t out_idx = x * 4;
    row_dst[out_idx + 0] = __float2half_rn(r);
    row_dst[out_idx + 1] = __float2half_rn(g);
    row_dst[out_idx + 2] = __float2half_rn(b);
    row_dst[out_idx + 3] = __float2half_rn(a);
}

void launch_unpack_rgb10a2_to_rgba16f(
    const void *src_packed, size_t src_pitch,
    void *dst_half4, size_t dst_pitch,
    uint32_t width, uint32_t height,
    cudaStream_t stream
) {
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    k_unpack_rgb10a2_to_rgba16f<<<grid, block, 0, stream>>>(
        reinterpret_cast<const uint8_t *>(src_packed), src_pitch,
        reinterpret_cast<uint8_t *>(dst_half4), dst_pitch,
        width, height
    );
}

__global__ void k_invert_rgba8(
    uint8_t *__restrict__ data, size_t pitch,
    uint32_t width, uint32_t height
) {
    uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    uchar4 *row = reinterpret_cast<uchar4 *>(data + y * pitch);
    uchar4 p = row[x];
    p.x = 255 - p.x;
    p.y = 255 - p.y;
    p.z = 255 - p.z;
    // alpha unchanged
    row[x] = p;
}

void launch_invert_rgba8(
    void *data, size_t pitch,
    uint32_t width, uint32_t height,
    cudaStream_t stream
) {
    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    k_invert_rgba8<<<grid, block, 0, stream>>>(
        reinterpret_cast<uint8_t *>(data), pitch,
        width, height
    );
}

} // namespace linrtx
