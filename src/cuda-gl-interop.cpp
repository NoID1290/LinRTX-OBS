#include "cuda-gl-interop.h"
#include <GL/gl.h>

namespace linrtx {

bool init_cuda_gl(int device_id) {
    cudaError_t err = cudaSetDevice(device_id);
    if (err != cudaSuccess) {
        blog(LOG_ERROR, "[LinRTX] Failed to set CUDA device %d: %s", device_id, cudaGetErrorString(err));
        return false;
    }
    // Initialize CUDA driver / runtime context
    err = cudaFree(nullptr);
    if (err != cudaSuccess) {
        blog(LOG_ERROR, "[LinRTX] Failed to initialize CUDA context: %s", cudaGetErrorString(err));
        return false;
    }
    return true;
}

CudaGLTextureBridge::CudaGLTextureBridge() = default;

CudaGLTextureBridge::~CudaGLTextureBridge() {
    unregister();
}

bool CudaGLTextureBridge::register_texture(gs_texture_t *gs_tex, uint32_t width, uint32_t height, unsigned int cuda_flags) {
    unregister();

    if (!gs_tex) {
        return false;
    }

    void *obj_ptr = gs_texture_get_obj(gs_tex);
    if (!obj_ptr) {
        blog(LOG_ERROR, "[LinRTX] gs_texture_get_obj returned NULL");
        return false;
    }

    m_gl_id = *static_cast<GLuint*>(obj_ptr);
    if (m_gl_id == 0) {
        blog(LOG_ERROR, "[LinRTX] Invalid OpenGL texture id: 0");
        return false;
    }

    cudaError_t err = cudaGraphicsGLRegisterImage(&m_resource, m_gl_id, GL_TEXTURE_2D, cuda_flags);
    if (err != cudaSuccess) {
        blog(LOG_ERROR, "[LinRTX] cudaGraphicsGLRegisterImage failed for GL texture %u: %s", m_gl_id, cudaGetErrorString(err));
        m_resource = nullptr;
        m_gl_id = 0;
        return false;
    }

    m_gs_tex = gs_tex;
    m_width = width;
    m_height = height;
    m_mapped = false;
    return true;
}

void CudaGLTextureBridge::unregister() {
    if (m_resource) {
        if (m_mapped) {
            cudaGraphicsUnmapResources(1, &m_resource, nullptr);
            m_mapped = false;
        }
        cudaGraphicsUnregisterResource(m_resource);
        m_resource = nullptr;
    }
    m_gs_tex = nullptr;
    m_gl_id = 0;
    m_width = 0;
    m_height = 0;
}

cudaArray_t CudaGLTextureBridge::map(cudaStream_t stream) {
    if (!m_resource) {
        return nullptr;
    }

    cudaError_t err = cudaGraphicsMapResources(1, &m_resource, stream);
    if (err != cudaSuccess) {
        blog(LOG_ERROR, "[LinRTX] cudaGraphicsMapResources failed: %s", cudaGetErrorString(err));
        return nullptr;
    }
    m_mapped = true;

    cudaArray_t arr = nullptr;
    err = cudaGraphicsSubResourceGetMappedArray(&arr, m_resource, 0, 0);
    if (err != cudaSuccess) {
        blog(LOG_ERROR, "[LinRTX] cudaGraphicsSubResourceGetMappedArray failed: %s", cudaGetErrorString(err));
        cudaGraphicsUnmapResources(1, &m_resource, stream);
        m_mapped = false;
        return nullptr;
    }
    return arr;
}

void CudaGLTextureBridge::unmap(cudaStream_t stream) {
    if (m_resource && m_mapped) {
        cudaError_t err = cudaGraphicsUnmapResources(1, &m_resource, stream);
        if (err != cudaSuccess) {
            blog(LOG_ERROR, "[LinRTX] cudaGraphicsUnmapResources failed: %s", cudaGetErrorString(err));
        }
        m_mapped = false;
    }
}

bool PitchedBuffer::allocate(size_t w_bytes, size_t h) {
    if (ptr && width_bytes == w_bytes && height == h) {
        return true;
    }
    free();

    width_bytes = w_bytes;
    height = h;
    cudaError_t err = cudaMallocPitch(&ptr, &pitch, width_bytes, height);
    if (err != cudaSuccess) {
        blog(LOG_ERROR, "[LinRTX] cudaMallocPitch failed (%zu x %zu): %s", width_bytes, height, cudaGetErrorString(err));
        ptr = nullptr;
        pitch = 0;
        return false;
    }
    return true;
}

void PitchedBuffer::free() {
    if (ptr) {
        cudaFree(ptr);
        ptr = nullptr;
    }
    pitch = 0;
    width_bytes = 0;
    height = 0;
}

} // namespace linrtx
