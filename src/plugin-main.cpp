#include <obs-module.h>
#include "nvvfx-loader.h"
#include "cuda-gl-interop.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("linrtx-obs", "en-US")

extern struct obs_source_info vsr_filter_info;
extern struct obs_source_info truehdr_filter_info;
extern struct obs_source_info framegen_filter_info;

bool obs_module_load(void) {
    blog(LOG_INFO, "[LinRTX] LinRTX-OBS plugin loading (v1.0.0)...");

    // Initialize CUDA with OpenGL interop support on device 0
    if (!linrtx::init_cuda_gl(0)) {
        blog(LOG_WARNING, "[LinRTX] CUDA initialization failed. Filters will run in pass-through mode.");
    }

    // Attempt to load NVIDIA Video Effects SDK libraries
    if (linrtx::load_sdk()) {
        blog(LOG_INFO, "[LinRTX] NVIDIA VFX SDK loaded successfully from: %s", linrtx::get_sdk_path().c_str());
    } else {
        blog(LOG_WARNING, "[LinRTX] NVIDIA VFX SDK not found at startup: %s. Filters will report status in properties.",
             linrtx::get_sdk_error().c_str());
    }

    // Always register the configured filter sources so they appear in OBS UI
#if defined(ENABLE_VSR) || !defined(DISABLE_VSR)
    obs_register_source(&vsr_filter_info);
#endif
#if defined(ENABLE_TRUEHDR) || !defined(DISABLE_TRUEHDR)
    obs_register_source(&truehdr_filter_info);
#endif
#if defined(ENABLE_FRAMEGEN) || !defined(DISABLE_FRAMEGEN)
    obs_register_source(&framegen_filter_info);
#endif

    blog(LOG_INFO, "[LinRTX] Registered filters: 'rtx_vsr_filter', 'rtx_truehdr_filter', and 'rtx_framegen_filter'.");
    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[LinRTX] Unloading LinRTX-OBS plugin...");
    linrtx::unload_sdk();
    blog(LOG_INFO, "[LinRTX] LinRTX-OBS plugin unloaded.");
}

MODULE_EXPORT const char *obs_module_description(void) {
    return "NVIDIA RTX Video Super Resolution, TrueHDR, and Frame Generation filters for Linux OBS Studio";
}

MODULE_EXPORT const char *obs_module_name(void) {
    return "LinRTX-OBS";
}
