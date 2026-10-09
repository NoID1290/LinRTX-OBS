#include "nvvfx-loader.h"
#include <dlfcn.h>
#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace linrtx {

static NvVFXApi g_api;
static bool g_loaded = false;
static std::string g_loaded_path;
static std::string g_last_error;
static std::vector<void*> g_handles;

const NvVFXApi &vfx() {
    return g_api;
}

bool is_sdk_loaded() {
    return g_loaded;
}

const std::string &get_sdk_path() {
    return g_loaded_path;
}

const std::string &get_sdk_error() {
    return g_last_error;
}

std::string status_to_string(NvCV_Status status) {
    if (g_api.GetErrorString) {
        const char *err = g_api.GetErrorString(status);
        if (err && *err) {
            return std::string(err) + " (code " + std::to_string(status) + ")";
        }
    }
    return "Error code " + std::to_string(status);
}

static void *load_lib(const fs::path &dir, const std::string &soname) {
    fs::path full_path = dir / soname;
    if (!fs::exists(full_path)) {
        // Try fallback to just soname in system path
        void *h = dlopen(soname.c_str(), RTLD_NOW | RTLD_GLOBAL);
        return h;
    }
    void *h = dlopen(full_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    return h;
}

static bool try_load_from_directory(const fs::path &dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        return false;
    }

    fs::path core_vfx = dir / "libVideoFX.so";
    fs::path core_cv = dir / "libNVCVImage.so";
    if (!fs::exists(core_vfx) || !fs::exists(core_cv)) {
        return false;
    }

    // Sequence of dependencies to load with RTLD_GLOBAL in order
    const std::vector<std::string> deps = {
        "libnppc.so.12",
        "libnppial.so.12",
        "libnppicc.so.12",
        "libnppidei.so.12",
        "libnppif.so.12",
        "libnppig.so.12",
        "libnppim.so.12",
        "libnppist.so.12",
        "libnppitc.so.12",
        "libcudnn.so.9",
        "libNVCVImage.so",
        "libnvngxruntime.so",
        "libVideoFXLocal.so",
        "libVideoFX.so",
        "libnvVFXVideoSuperRes.so",
        "libnvidia-ngx-vsr.so.1.8.3",
        "libnvVFXTrueHDR.so",
        "libnvidia-ngx-truehdr.so.1.2.3",
        "libnvVFXVideoFrameGeneration.so"
    };

    std::vector<void*> handles;
    for (const auto &lib_name : deps) {
        void *h = load_lib(dir, lib_name);
        if (h) {
            handles.push_back(h);
        }
    }

    // Now resolve symbols from libVideoFX and libNVCVImage
    void *vfx_handle = dlopen((dir / "libVideoFX.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!vfx_handle) {
        g_last_error = "Failed to dlopen libVideoFX.so: " + std::string(dlerror());
        return false;
    }

    void *cv_handle = dlopen((dir / "libNVCVImage.so").c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!cv_handle) {
        g_last_error = "Failed to dlopen libNVCVImage.so: " + std::string(dlerror());
        dlclose(vfx_handle);
        return false;
    }

    // Resolve NvVFX functions
    #define RESOLVE_VFX(name) \
        g_api.name = reinterpret_cast<NvVFX_##name##_t>(dlsym(vfx_handle, "NvVFX_" #name)); \
        if (!g_api.name) { \
            g_last_error = "Missing symbol NvVFX_" #name; \
            return false; \
        }

    RESOLVE_VFX(GetVersion);
    RESOLVE_VFX(CreateEffect);
    RESOLVE_VFX(DestroyEffect);
    RESOLVE_VFX(SetU32);
    RESOLVE_VFX(SetS32);
    RESOLVE_VFX(SetF32);
    RESOLVE_VFX(SetString);
    RESOLVE_VFX(SetImage);
    RESOLVE_VFX(SetCudaStream);
    RESOLVE_VFX(GetU32);
    RESOLVE_VFX(GetF32);
    RESOLVE_VFX(GetString);
    RESOLVE_VFX(GetImage);
    RESOLVE_VFX(Load);
    RESOLVE_VFX(Run);
    RESOLVE_VFX(CudaStreamCreate);
    RESOLVE_VFX(CudaStreamDestroy);

    #undef RESOLVE_VFX

    // Resolve NvCVImage functions
    #define RESOLVE_CV(field, sym) \
        g_api.field = reinterpret_cast<sym##_t>(dlsym(cv_handle, #sym)); \
        if (!g_api.field) { \
            g_last_error = "Missing symbol " #sym; \
            return false; \
        }

    RESOLVE_CV(GetErrorString, NvCV_GetErrorStringFromCode);
    RESOLVE_CV(ImageInit, NvCVImage_Init);
    RESOLVE_CV(ImageAlloc, NvCVImage_Alloc);
    RESOLVE_CV(ImageDealloc, NvCVImage_Dealloc);
    RESOLVE_CV(ImageTransfer, NvCVImage_Transfer);

    #undef RESOLVE_CV

    g_handles = std::move(handles);
    g_handles.push_back(vfx_handle);
    g_handles.push_back(cv_handle);
    g_loaded_path = dir.string();
    g_loaded = true;
    g_last_error.clear();
    return true;
}

bool load_sdk(const char *explicit_path) {
    if (g_loaded) return true;

    std::vector<fs::path> candidates;

    if (explicit_path && *explicit_path) {
        candidates.push_back(explicit_path);
    }

    const char *env_path = std::getenv("LINRTX_SDK_PATH");
    if (env_path && *env_path) {
        candidates.push_back(env_path);
    }

    // Standard user home python / venv locations
    const char *home = std::getenv("HOME");
    if (home) {
        fs::path p_home(home);
        candidates.push_back(p_home / "ComfyUI/.venv/lib/python3.12/site-packages/nvvfx/libs");
        candidates.push_back(p_home / "ComfyUI/.venv/lib/python3.11/site-packages/nvvfx/libs");
        candidates.push_back(p_home / ".local/lib/python3.12/site-packages/nvvfx/libs");
        candidates.push_back(p_home / ".local/lib/python3.11/site-packages/nvvfx/libs");
    }

    // System standard paths
    candidates.push_back("/usr/local/VideoFX/lib");
    candidates.push_back("/opt/nvidia/vfx/lib");
    candidates.push_back("/opt/nvidia/libnvvfx");
    candidates.push_back("/usr/lib64/nvvfx");

    for (const auto &path : candidates) {
        if (try_load_from_directory(path)) {
            return true;
        }
    }

    if (g_last_error.empty()) {
        g_last_error = "NVIDIA VFX SDK libraries (libVideoFX.so, libNVCVImage.so) not found in standard paths.";
    }
    return false;
}

void unload_sdk() {
    for (void *h : g_handles) {
        if (h) dlclose(h);
    }
    g_handles.clear();
    g_api = {};
    g_loaded = false;
    g_loaded_path.clear();
}

} // namespace linrtx
