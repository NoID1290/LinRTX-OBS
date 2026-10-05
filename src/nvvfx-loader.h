#pragma once

#include "nvvfx-interface.h"
#include <string>

namespace linrtx {

struct NvVFXApi {
    NvVFX_GetVersion_t GetVersion = nullptr;
    NvVFX_CreateEffect_t CreateEffect = nullptr;
    NvVFX_DestroyEffect_t DestroyEffect = nullptr;
    NvVFX_SetU32_t SetU32 = nullptr;
    NvVFX_SetS32_t SetS32 = nullptr;
    NvVFX_SetF32_t SetF32 = nullptr;
    NvVFX_SetString_t SetString = nullptr;
    NvVFX_SetImage_t SetImage = nullptr;
    NvVFX_SetCudaStream_t SetCudaStream = nullptr;

    NvVFX_GetU32_t GetU32 = nullptr;
    NvVFX_GetF32_t GetF32 = nullptr;
    NvVFX_GetString_t GetString = nullptr;
    NvVFX_GetImage_t GetImage = nullptr;

    NvVFX_Load_t Load = nullptr;
    NvVFX_Run_t Run = nullptr;
    NvVFX_CudaStreamCreate_t CudaStreamCreate = nullptr;
    NvVFX_CudaStreamDestroy_t CudaStreamDestroy = nullptr;

    NvCV_GetErrorStringFromCode_t GetErrorString = nullptr;
    NvCVImage_Init_t ImageInit = nullptr;
    NvCVImage_Alloc_t ImageAlloc = nullptr;
    NvCVImage_Dealloc_t ImageDealloc = nullptr;
    NvCVImage_Transfer_t ImageTransfer = nullptr;
};

// Global accessor
const NvVFXApi &vfx();

bool is_sdk_loaded();
const std::string &get_sdk_path();
const std::string &get_sdk_error();

// Attempts to discover and load the NVIDIA VFX SDK
bool load_sdk(const char *explicit_path = nullptr);
void unload_sdk();

// Helper to format NvCV_Status as string
std::string status_to_string(NvCV_Status status);

} // namespace linrtx
