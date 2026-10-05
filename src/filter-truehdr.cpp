#include "nvvfx-loader.h"
#include "cuda-gl-interop.h"
#include "kernels/format_convert.h"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <chrono>
#include <string>

#define S_LUMINANCE     "luminance"
#define S_CONTRAST      "contrast"
#define S_SATURATION    "saturation"
#define S_MIDDLE_GRAY   "middle_gray"
#define S_STATUS        "status_info"

struct truehdr_filter_data {
    obs_source_t *context = nullptr;
    gs_effect_t *effect = nullptr;
    gs_eparam_t *image_param = nullptr;
    gs_eparam_t *sdr_white_param = nullptr;

    // Filter tunables
    int luminance = 1000;
    int contrast = 100;
    int saturation = 100;
    int middle_gray = 50;

    // Dimensions
    uint32_t width = 0;
    uint32_t height = 0;

    // Graphics & CUDA interop
    gs_texrender_t *texrender_in = nullptr;
    gs_texture_t *texture_out = nullptr;
    linrtx::CudaGLTextureBridge bridge_in;
    linrtx::CudaGLTextureBridge bridge_out;
    linrtx::PitchedBuffer buf_in;
    linrtx::PitchedBuffer buf_out;
    linrtx::PitchedBuffer buf_out_fp16;

    // VFX SDK state
    NvVFX_Handle vfx_handle = nullptr;
    CUstream stream = nullptr;
    NvCVImage src_img = {};
    NvCVImage dst_img = {};
    bool vfx_initialized = false;
    bool needs_reload = false;

    // Performance tracking
    float last_ms = 0.0f;
    float last_fps = 0.0f;
    std::string status_msg;
};

static const char *truehdr_get_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return obs_module_text("RTXTrueHDR");
}

static void update_hdr_status_text(truehdr_filter_data *filter) {
    if (!linrtx::is_sdk_loaded()) {
        filter->status_msg = "SDK Status: NOT FOUND (" + linrtx::get_sdk_error() + ")";
        return;
    }
    if (!filter->vfx_initialized) {
        filter->status_msg = "SDK: Ready | Model: Initializing...";
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "SDK: Ready | Peak: %d nits | GPU: %.2f ms (%.1f FPS)",
             filter->luminance, filter->last_ms, filter->last_fps);
    filter->status_msg = buf;
}

static void truehdr_destroy_vfx(truehdr_filter_data *filter) {
    const auto &api = linrtx::vfx();
    if (filter->vfx_handle) {
        api.DestroyEffect(filter->vfx_handle);
        filter->vfx_handle = nullptr;
    }
    if (filter->stream) {
        api.CudaStreamDestroy(filter->stream);
        filter->stream = nullptr;
    }
    filter->buf_in.free();
    filter->buf_out.free();
    filter->buf_out_fp16.free();
    filter->vfx_initialized = false;
}

static bool truehdr_init_vfx(truehdr_filter_data *filter) {
    if (!linrtx::is_sdk_loaded()) {
        return false;
    }
    const auto &api = linrtx::vfx();

    if (!filter->stream) {
        NvCV_Status err = api.CudaStreamCreate(&filter->stream);
        if (err != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[LinRTX TrueHDR] Failed to create CUDA stream: %s", linrtx::status_to_string(err).c_str());
            return false;
        }
    }

    if (!filter->vfx_handle) {
        NvCV_Status err = api.CreateEffect(NVVFX_FX_TRUE_HDR, &filter->vfx_handle);
        if (err != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[LinRTX TrueHDR] Failed to create TrueHDR effect: %s", linrtx::status_to_string(err).c_str());
            return false;
        }
    }

    // Input buffer: RGBA U8 (4 bytes/pixel)
    if (!filter->buf_in.allocate(filter->width * 4, filter->height)) {
        return false;
    }
    // Output buffer: RGB10A2 P32 (packed uint32, 4 bytes/pixel)
    if (!filter->buf_out.allocate(filter->width * 4, filter->height)) {
        return false;
    }
    // Intermediate buffer for format conversion: RGBA16F (8 bytes/pixel: 4 channels * half)
    if (!filter->buf_out_fp16.allocate(filter->width * sizeof(uint16_t) * 4, filter->height)) {
        return false;
    }

    api.ImageInit(&filter->src_img, filter->width, filter->height,
                  static_cast<int>(filter->buf_in.pitch), filter->buf_in.ptr,
                  NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA);

    api.ImageInit(&filter->dst_img, filter->width, filter->height,
                  static_cast<int>(filter->buf_out.pitch), filter->buf_out.ptr,
                  NVCV_RGB10A2, NVCV_P32, NVCV_CHUNKY, NVCV_CUDA);

    api.SetCudaStream(filter->vfx_handle, NVVFX_PARAM_CUDA_STREAM, filter->stream);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_LUMINANCE, filter->luminance);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_CONTRAST, filter->contrast);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_SATURATION, filter->saturation);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_MIDDLE_GRAY, filter->middle_gray);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_DEBANDING_OFF, 0);

    api.SetImage(filter->vfx_handle, NVVFX_PARAM_SRC_IMAGE_0, &filter->src_img);
    api.SetImage(filter->vfx_handle, NVVFX_PARAM_DST_IMAGE_0, &filter->dst_img);

    NvCV_Status err = api.Load(filter->vfx_handle);
    if (err != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[LinRTX TrueHDR] NvVFX_Load failed: %s", linrtx::status_to_string(err).c_str());
        return false;
    }

    filter->vfx_initialized = true;
    filter->needs_reload = false;
    blog(LOG_INFO, "[LinRTX TrueHDR] Initialized %ux%u, Peak Luminance %d nits",
         filter->width, filter->height, filter->luminance);
    return true;
}

static void truehdr_update(void *data, obs_data_t *settings) {
    auto *filter = static_cast<truehdr_filter_data *>(data);

    filter->luminance = static_cast<int>(obs_data_get_int(settings, S_LUMINANCE));
    filter->contrast = static_cast<int>(obs_data_get_int(settings, S_CONTRAST));
    filter->saturation = static_cast<int>(obs_data_get_int(settings, S_SATURATION));
    filter->middle_gray = static_cast<int>(obs_data_get_int(settings, S_MIDDLE_GRAY));

    if (filter->vfx_initialized && filter->vfx_handle) {
        const auto &api = linrtx::vfx();
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_LUMINANCE, filter->luminance);
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_CONTRAST, filter->contrast);
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_SATURATION, filter->saturation);
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_MIDDLE_GRAY, filter->middle_gray);
    }
}

static void *truehdr_create(obs_data_t *settings, obs_source_t *context) {
    auto *filter = new truehdr_filter_data();
    filter->context = context;

    obs_enter_graphics();
    char *effect_path = obs_module_file("effects/linrtx_hdr.effect");
    if (effect_path) {
        filter->effect = gs_effect_create_from_file(effect_path, nullptr);
        bfree(effect_path);
    }
    if (!filter->effect) {
        blog(LOG_ERROR, "[LinRTX TrueHDR] Failed to load effects/linrtx_hdr.effect");
    } else {
        filter->image_param = gs_effect_get_param_by_name(filter->effect, "image");
        filter->sdr_white_param = gs_effect_get_param_by_name(filter->effect, "sdr_white_level");
    }
    obs_leave_graphics();

    linrtx::load_sdk();
    truehdr_update(filter, settings);
    return filter;
}

static void truehdr_destroy(void *data) {
    auto *filter = static_cast<truehdr_filter_data *>(data);
    if (!filter) return;

    obs_enter_graphics();
    filter->bridge_in.unregister();
    filter->bridge_out.unregister();
    if (filter->texrender_in) {
        gs_texrender_destroy(filter->texrender_in);
        filter->texrender_in = nullptr;
    }
    if (filter->texture_out) {
        gs_texture_destroy(filter->texture_out);
        filter->texture_out = nullptr;
    }
    if (filter->effect) {
        gs_effect_destroy(filter->effect);
        filter->effect = nullptr;
    }
    obs_leave_graphics();

    truehdr_destroy_vfx(filter);
    delete filter;
}

static void truehdr_video_tick(void *data, float seconds) {
    UNUSED_PARAMETER(seconds);
    auto *filter = static_cast<truehdr_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) return;

    uint32_t cur_w = obs_source_get_base_width(target);
    uint32_t cur_h = obs_source_get_base_height(target);
    if (cur_w == 0 || cur_h == 0) return;

    if (cur_w != filter->width || cur_h != filter->height || filter->needs_reload) {
        filter->width = cur_w;
        filter->height = cur_h;
        filter->needs_reload = true;
    }
}

static enum gs_color_space truehdr_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces) {
    UNUSED_PARAMETER(data);
    // Prefer scRGB (standard for Windows/Linux HDR) or 709_EXTENDED if requested
    for (size_t i = 0; i < count; ++i) {
        if (preferred_spaces[i] == GS_CS_709_SCRGB) {
            return GS_CS_709_SCRGB;
        }
        if (preferred_spaces[i] == GS_CS_709_EXTENDED) {
            return GS_CS_709_EXTENDED;
        }
    }
    // If canvas is SDR, report GS_CS_SRGB
    for (size_t i = 0; i < count; ++i) {
        if (preferred_spaces[i] == GS_CS_SRGB) {
            return GS_CS_SRGB;
        }
    }
    return count > 0 ? preferred_spaces[0] : GS_CS_SRGB;
}

static void truehdr_video_render(void *data, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);
    auto *filter = static_cast<truehdr_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);

    if (!target || filter->width == 0 || filter->height == 0) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    if (!linrtx::is_sdk_loaded() || !filter->effect) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Reallocate textures if dimensions changed
    if (filter->needs_reload || !filter->vfx_initialized) {
        obs_enter_graphics();
        filter->bridge_in.unregister();
        filter->bridge_out.unregister();

        if (filter->texrender_in) {
            gs_texrender_destroy(filter->texrender_in);
        }
        filter->texrender_in = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

        if (filter->texture_out) {
            gs_texture_destroy(filter->texture_out);
        }
        // Output texture is GS_RGBA16F (supported by CUDA-GL interop and HDR canvas)
        filter->texture_out = gs_texture_create(filter->width, filter->height, GS_RGBA16F, 1, nullptr, GS_DYNAMIC);
        obs_leave_graphics();

        truehdr_destroy_vfx(filter);
        if (!truehdr_init_vfx(filter)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Step 1: Render SDR source into input gs_texrender (in sRGB space)
    gs_texrender_reset(filter->texrender_in);
    if (!gs_texrender_begin(filter->texrender_in, filter->width, filter->height)) {
        obs_source_skip_video_filter(filter->context);
        return;
    }
    gs_ortho(0.0f, static_cast<float>(filter->width), 0.0f, static_cast<float>(filter->height), -100.0f, 100.0f);
    obs_source_video_render(target);
    gs_texrender_end(filter->texrender_in);

    gs_texture_t *in_tex = gs_texrender_get_texture(filter->texrender_in);
    if (!in_tex || !filter->texture_out) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    if (!filter->bridge_in.is_registered()) {
        if (!filter->bridge_in.register_texture(in_tex, filter->width, filter->height, cudaGraphicsRegisterFlagsReadOnly)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }
    if (!filter->bridge_out.is_registered()) {
        if (!filter->bridge_out.register_texture(filter->texture_out, filter->width, filter->height, cudaGraphicsRegisterFlagsWriteDiscard)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Step 2: Zero-copy CUDA-GL map
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(filter->stream);
    cudaArray_t arr_in = filter->bridge_in.map(stream);
    cudaArray_t arr_out = filter->bridge_out.map(stream);

    if (arr_in && arr_out) {
        // Copy RGBA U8 input to linear buffer
        cudaMemcpy2DFromArrayAsync(
            filter->buf_in.ptr, filter->buf_in.pitch,
            arr_in, 0, 0,
            filter->width * 4, filter->height,
            cudaMemcpyDeviceToDevice, stream
        );

        // Run TrueHDR inference
        auto t0 = std::chrono::high_resolution_clock::now();
        const auto &api = linrtx::vfx();
        NvCV_Status run_err = api.Run(filter->vfx_handle, 0);
        cudaStreamSynchronize(stream);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (run_err == NVCV_SUCCESS) {
            float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            filter->last_ms = ms;
            filter->last_fps = (ms > 0.001f) ? (1000.0f / ms) : 0.0f;

            // Unpack packed RGB10A2 P32 into half4 (FP16 RGBA)
            linrtx::launch_unpack_rgb10a2_to_rgba16f(
                filter->buf_out.ptr, filter->buf_out.pitch,
                filter->buf_out_fp16.ptr, filter->buf_out_fp16.pitch,
                filter->width, filter->height,
                stream
            );

            // Copy half4 FP16 RGBA to output GL texture array
            cudaMemcpy2DToArrayAsync(
                arr_out, 0, 0,
                filter->buf_out_fp16.ptr, filter->buf_out_fp16.pitch,
                filter->width * sizeof(uint16_t) * 4, filter->height,
                cudaMemcpyDeviceToDevice, stream
            );
        } else {
            blog(LOG_ERROR, "[LinRTX TrueHDR] NvVFX_Run error: %s", linrtx::status_to_string(run_err).c_str());
        }
    }

    filter->bridge_in.unmap(stream);
    filter->bridge_out.unmap(stream);

    // Step 3: Determine target color space and technique
    enum gs_color_space current_space = gs_get_color_space();
    const char *technique = "DrawSDR_Tonemap";
    enum gs_color_format draw_format = GS_RGBA;

    if (current_space == GS_CS_709_SCRGB) {
        technique = "DrawHDR_scRGB";
        draw_format = GS_RGBA16F;
    } else if (current_space == GS_CS_709_EXTENDED) {
        technique = "DrawHDR_Extended";
        draw_format = GS_RGBA16F;
    }

    if (obs_source_process_filter_begin_with_color_space(filter->context, draw_format, current_space, OBS_NO_DIRECT_RENDERING)) {
        gs_effect_set_texture(filter->image_param, filter->texture_out);
        if (filter->sdr_white_param) {
            gs_effect_set_float(filter->sdr_white_param, obs_get_video_sdr_white_level());
        }
        obs_source_process_filter_tech_end(filter->context, filter->effect, filter->width, filter->height, technique);
    }
}

static uint32_t truehdr_get_width(void *data) {
    auto *filter = static_cast<truehdr_filter_data *>(data);
    return filter->width;
}

static uint32_t truehdr_get_height(void *data) {
    auto *filter = static_cast<truehdr_filter_data *>(data);
    return filter->height;
}

static void truehdr_get_defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, S_LUMINANCE, 1000);
    obs_data_set_default_int(settings, S_CONTRAST, 100);
    obs_data_set_default_int(settings, S_SATURATION, 100);
    obs_data_set_default_int(settings, S_MIDDLE_GRAY, 50);
    obs_data_set_default_string(settings, S_STATUS, "Ready");
}

static obs_properties_t *truehdr_get_properties(void *data) {
    auto *filter = static_cast<truehdr_filter_data *>(data);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_int_slider(props, S_LUMINANCE, obs_module_text("PeakLuminance"), 400, 2000, 50);
    obs_properties_add_int_slider(props, S_CONTRAST, obs_module_text("Contrast"), 0, 200, 1);
    obs_properties_add_int_slider(props, S_SATURATION, obs_module_text("Saturation"), 0, 200, 1);
    obs_properties_add_int_slider(props, S_MIDDLE_GRAY, obs_module_text("MiddleGray"), 10, 100, 1);

    if (filter) {
        update_hdr_status_text(filter);
    }
    obs_property_t *status = obs_properties_add_text(props, S_STATUS, obs_module_text("Status"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(status, false);

    return props;
}

struct obs_source_info truehdr_filter_info = {
    .id = "rtx_truehdr_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = truehdr_get_name,
    .create = truehdr_create,
    .destroy = truehdr_destroy,
    .get_width = truehdr_get_width,
    .get_height = truehdr_get_height,
    .get_defaults = truehdr_get_defaults,
    .get_properties = truehdr_get_properties,
    .update = truehdr_update,
    .video_tick = truehdr_video_tick,
    .video_render = truehdr_video_render,
    .video_get_color_space = truehdr_get_color_space,
};
