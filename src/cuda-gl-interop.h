#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>
#include <cstdint>
#include <string>

namespace linrtx {

bool init_cuda_gl(int device_id = 0);

class CudaGLTextureBridge {
public:
    CudaGLTextureBridge();
    ~CudaGLTextureBridge();

    // Registers an OBS gs_texture_t for zero-copy CUDA access
    bool register_texture(gs_texture_t *gs_tex, uint32_t width, uint32_t height, unsigned int cuda_flags);
    void unregister();

    // Map the registered texture to obtain cudaArray_t
    cudaArray_t map(cudaStream_t stream);
    void unmap(cudaStream_t stream);

    bool is_registered() const { return m_resource != nullptr; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    GLuint gl_texture_id() const { return m_gl_id; }
    gs_texture_t *gs_texture() const { return m_gs_tex; }

private:
    gs_texture_t *m_gs_tex = nullptr;
    GLuint m_gl_id = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    cudaGraphicsResource_t m_resource = nullptr;
    bool m_mapped = false;
};

// Helper struct for managing a 2D pitched CUDA linear buffer
struct PitchedBuffer {
    void *ptr = nullptr;
    size_t pitch = 0;
    size_t width_bytes = 0;
    size_t height = 0;

    bool allocate(size_t w_bytes, size_t h);
    void free();
};

} // namespace linrtx
