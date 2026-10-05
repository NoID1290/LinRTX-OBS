#include "nvvfx-loader.h"
#include "cuda-gl-interop.h"
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <chrono>
#include <string>

#define S_SCALE_MODE        "scale_mode"
#define S_SCALE_MULTIPLIER  "scale_multiplier"
#define S_TARGET_RES        "target_res"
#define S_QUALITY           "quality"
#define S_STATUS            "status_info"

struct vsr_filter_data {
    obs_source_t *context = nullptr;
    gs_effect_t *effect = nullptr;
    gs_eparam_t *image_param = nullptr;

    // Filter settings
    int scale_mode = 0; // 0 = Multiplier, 1 = Target resolution
    float scale_multiplier = 2.0f;
    int target_width = 1920;
    int target_height = 1080;
    int quality_level = 4; // Ultra

    // Dimensions
    uint32_t in_width = 0;
    uint32_t in_height = 0;
    uint32_t out_width = 0;
    uint32_t out_height = 0;

    // Graphics & CUDA interop
    gs_texrender_t *texrender_in = nullptr;
    gs_texture_t *texture_out = nullptr;
    linrtx::CudaGLTextureBridge bridge_in;
    linrtx::CudaGLTextureBridge bridge_out;
    linrtx::PitchedBuffer buf_in;
    linrtx::PitchedBuffer buf_out;

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

static const char *vsr_get_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return obs_module_text("RTXVideoSuperResolution");
}

static void update_status_text(vsr_filter_data *filter) {
    if (!linrtx::is_sdk_loaded()) {
        filter->status_msg = "SDK Status: NOT FOUND (" + linrtx::get_sdk_error() + ")";
        return;
    }
    if (!filter->vfx_initialized) {
        filter->status_msg = "SDK: Ready | Model: Initializing...";
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "SDK: Ready | %ux%u -> %ux%u | GPU: %.2f ms (%.1f FPS)",
             filter->in_width, filter->in_height, filter->out_width, filter->out_height,
             filter->last_ms, filter->last_fps);
    filter->status_msg = buf;
}

static void vsr_destroy_vfx(vsr_filter_data *filter) {
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
    filter->vfx_initialized = false;
}

static bool vsr_init_vfx(vsr_filter_data *filter) {
    if (!linrtx::is_sdk_loaded()) {
        return false;
    }
    const auto &api = linrtx::vfx();

    if (!filter->stream) {
        NvCV_Status err = api.CudaStreamCreate(&filter->stream);
        if (err != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[LinRTX VSR] Failed to create CUDA stream: %s", linrtx::status_to_string(err).c_str());
            return false;
        }
    }

    if (!filter->vfx_handle) {
        NvCV_Status err = api.CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &filter->vfx_handle);
        if (err != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[LinRTX VSR] Failed to create VideoSuperRes effect: %s", linrtx::status_to_string(err).c_str());
            return false;
        }
    }

    // Allocate linear pitched buffers for CUDA
    if (!filter->buf_in.allocate(filter->in_width * 4, filter->in_height)) {
        return false;
    }
    if (!filter->buf_out.allocate(filter->out_width * 4, filter->out_height)) {
        return false;
    }

    // Initialize NvCVImage wrappers pointing to pitched buffers
    api.ImageInit(&filter->src_img, filter->in_width, filter->in_height,
                  static_cast<int>(filter->buf_in.pitch), filter->buf_in.ptr,
                  NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA);

    api.ImageInit(&filter->dst_img, filter->out_width, filter->out_height,
                  static_cast<int>(filter->buf_out.pitch), filter->buf_out.ptr,
                  NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA);

    api.SetCudaStream(filter->vfx_handle, NVVFX_PARAM_CUDA_STREAM, filter->stream);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_IMAGE_ENCODING, 0); // 8-bit RGBA
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_QUALITY_LEVEL, filter->quality_level);
    api.SetImage(filter->vfx_handle, NVVFX_PARAM_SRC_IMAGE_0, &filter->src_img);
    api.SetImage(filter->vfx_handle, NVVFX_PARAM_DST_IMAGE_0, &filter->dst_img);

    NvCV_Status err = api.Load(filter->vfx_handle);
    if (err != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[LinRTX VSR] NvVFX_Load failed: %s", linrtx::status_to_string(err).c_str());
        return false;
    }

    filter->vfx_initialized = true;
    filter->needs_reload = false;
    blog(LOG_INFO, "[LinRTX VSR] Initialized %ux%u -> %ux%u, quality level %d",
         filter->in_width, filter->in_height, filter->out_width, filter->out_height, filter->quality_level);
    return true;
}

static void vsr_update(void *data, obs_data_t *settings) {
    auto *filter = static_cast<vsr_filter_data *>(data);

    int new_mode = static_cast<int>(obs_data_get_int(settings, S_SCALE_MODE));
    float new_mult = static_cast<float>(obs_data_get_double(settings, S_SCALE_MULTIPLIER));
    int new_q = static_cast<int>(obs_data_get_int(settings, S_QUALITY));
    int new_tw = filter->target_width;
    int new_th = filter->target_height;

    const char *target_res_str = obs_data_get_string(settings, S_TARGET_RES);
    if (target_res_str && *target_res_str) {
        int w = 1920, h = 1080;
        if (sscanf(target_res_str, "%dx%d", &w, &h) == 2) {
            new_tw = w;
            new_th = h;
        }
    }

    if (new_mode != filter->scale_mode ||
        new_mult != filter->scale_multiplier ||
        new_q != filter->quality_level ||
        new_tw != filter->target_width ||
        new_th != filter->target_height) {
        filter->scale_mode = new_mode;
        filter->scale_multiplier = new_mult;
        filter->quality_level = new_q;
        filter->target_width = new_tw;
        filter->target_height = new_th;
        filter->needs_reload = true;
    }
}

static void *vsr_create(obs_data_t *settings, obs_source_t *context) {
    auto *filter = new vsr_filter_data();
    filter->context = context;

    obs_enter_graphics();
    filter->effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    filter->image_param = gs_effect_get_param_by_name(filter->effect, "image");
    obs_leave_graphics();

    linrtx::load_sdk();
    vsr_update(filter, settings);
    return filter;
}

static void vsr_destroy(void *data) {
    auto *filter = static_cast<vsr_filter_data *>(data);
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
    obs_leave_graphics();

    vsr_destroy_vfx(filter);
    delete filter;
}

static void vsr_video_tick(void *data, float seconds) {
    UNUSED_PARAMETER(seconds);
    auto *filter = static_cast<vsr_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) return;

    uint32_t cur_w = obs_source_get_base_width(target);
    uint32_t cur_h = obs_source_get_base_height(target);
    if (cur_w == 0 || cur_h == 0) return;

    uint32_t calc_out_w = cur_w;
    uint32_t calc_out_h = cur_h;

    if (filter->quality_level >= 8 && filter->quality_level <= 13) {
        // Denoise/Deblur modes are 1x-only cleanup passes
        calc_out_w = cur_w;
        calc_out_h = cur_h;
    } else if (filter->scale_mode == 0) {
        calc_out_w = static_cast<uint32_t>(cur_w * filter->scale_multiplier);
        calc_out_h = static_cast<uint32_t>(cur_h * filter->scale_multiplier);
    } else {
        calc_out_w = filter->target_width;
        calc_out_h = filter->target_height;
    }
    // 4-pixel alignment for optimal GPU memory pitch
    calc_out_w = (calc_out_w + 3) & ~3;
    calc_out_h = (calc_out_h + 3) & ~3;

    if (cur_w != filter->in_width || cur_h != filter->in_height ||
        calc_out_w != filter->out_width || calc_out_h != filter->out_height ||
        filter->needs_reload) {
        filter->in_width = cur_w;
        filter->in_height = cur_h;
        filter->out_width = calc_out_w;
        filter->out_height = calc_out_h;
        filter->needs_reload = true;
    }
}

static void vsr_video_render(void *data, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);
    auto *filter = static_cast<vsr_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);

    if (!target || filter->in_width == 0 || filter->in_height == 0) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Pass through if SDK missing
    if (!linrtx::is_sdk_loaded()) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Reallocate GL textures and VFX effect on resolution change
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
        filter->texture_out = gs_texture_create(filter->out_width, filter->out_height, GS_RGBA, 1, nullptr, GS_DYNAMIC);
        obs_leave_graphics();

        vsr_destroy_vfx(filter);
        if (!vsr_init_vfx(filter)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Step 1: Render source into input gs_texrender
    gs_texrender_reset(filter->texrender_in);
    if (!gs_texrender_begin(filter->texrender_in, filter->in_width, filter->in_height)) {
        obs_source_skip_video_filter(filter->context);
        return;
    }
    struct vec4 clear_color = {};
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f, static_cast<float>(filter->in_width), 0.0f, static_cast<float>(filter->in_height), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_video_render(target);
    gs_blend_state_pop();
    gs_texrender_end(filter->texrender_in);

    gs_texture_t *in_tex = gs_texrender_get_texture(filter->texrender_in);
    if (!in_tex || !filter->texture_out) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Register textures with CUDA if not already registered or if texture changed
    if (!filter->bridge_in.is_registered() || filter->bridge_in.gs_texture() != in_tex) {
        if (!filter->bridge_in.register_texture(in_tex, filter->in_width, filter->in_height, cudaGraphicsRegisterFlagsReadOnly)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }
    if (!filter->bridge_out.is_registered() || filter->bridge_out.gs_texture() != filter->texture_out) {
        if (!filter->bridge_out.register_texture(filter->texture_out, filter->out_width, filter->out_height, cudaGraphicsRegisterFlagsWriteDiscard)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Step 2: Zero-copy CUDA-GL map
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(filter->stream);
    cudaArray_t arr_in = filter->bridge_in.map(stream);
    cudaArray_t arr_out = filter->bridge_out.map(stream);

    if (arr_in && arr_out) {
        // Step 3: Copy input GL texture array -> linear CUDA buffer
        cudaMemcpy2DFromArrayAsync(
            filter->buf_in.ptr, filter->buf_in.pitch,
            arr_in, 0, 0,
            filter->in_width * 4, filter->in_height,
            cudaMemcpyDeviceToDevice, stream
        );

        // Step 4: Run AI Super Resolution
        auto t0 = std::chrono::high_resolution_clock::now();
        const auto &api = linrtx::vfx();
        NvCV_Status run_err = api.Run(filter->vfx_handle, 0);
        cudaStreamSynchronize(stream);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (run_err == NVCV_SUCCESS) {
            float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
            filter->last_ms = ms;
            filter->last_fps = (ms > 0.001f) ? (1000.0f / ms) : 0.0f;

            // Step 5: Copy output linear CUDA buffer -> output GL texture array
            cudaMemcpy2DToArrayAsync(
                arr_out, 0, 0,
                filter->buf_out.ptr, filter->buf_out.pitch,
                filter->out_width * 4, filter->out_height,
                cudaMemcpyDeviceToDevice, stream
            );
        } else {
            blog(LOG_ERROR, "[LinRTX VSR] NvVFX_Run error: %s", linrtx::status_to_string(run_err).c_str());
        }
    }

    filter->bridge_in.unmap(stream);
    filter->bridge_out.unmap(stream);

    // Step 6: Render upscaled output texture directly to the active render target
    const bool previous_linear = gs_set_linear_srgb(true);
    const bool linear_srgb = gs_get_linear_srgb();
    const bool prev_fbo_srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(linear_srgb);

    if (linear_srgb) {
        gs_effect_set_texture_srgb(filter->image_param, filter->texture_out);
    } else {
        gs_effect_set_texture(filter->image_param, filter->texture_out);
    }

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    while (gs_effect_loop(filter->effect, "Draw")) {
        gs_draw_sprite(filter->texture_out, 0, filter->out_width, filter->out_height);
    }

    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(prev_fbo_srgb);
    gs_set_linear_srgb(previous_linear);
}

static uint32_t vsr_get_width(void *data) {
    auto *filter = static_cast<vsr_filter_data *>(data);
    return filter->out_width ? filter->out_width : filter->in_width;
}

static uint32_t vsr_get_height(void *data) {
    auto *filter = static_cast<vsr_filter_data *>(data);
    return filter->out_height ? filter->out_height : filter->in_height;
}

static enum gs_color_space vsr_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces) {
    auto *filter = static_cast<vsr_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);
    const enum gs_color_space potential_spaces[] = {
        GS_CS_SRGB,
        GS_CS_SRGB_16F,
        GS_CS_709_EXTENDED,
    };
    enum gs_color_space source_space = target ?
        obs_source_get_color_space(target, OBS_COUNTOF(potential_spaces), potential_spaces) : GS_CS_SRGB;

    for (size_t i = 0; i < count; ++i) {
        if (preferred_spaces[i] == source_space) {
            return source_space;
        }
    }
    return count > 0 ? preferred_spaces[0] : GS_CS_SRGB;
}

static void vsr_get_defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, S_SCALE_MODE, 0); // Multiplier
    obs_data_set_default_double(settings, S_SCALE_MULTIPLIER, 2.0);
    obs_data_set_default_string(settings, S_TARGET_RES, "3840x2160");
    obs_data_set_default_int(settings, S_QUALITY, 4); // Ultra
    obs_data_set_default_string(settings, S_STATUS, "Ready");
}

static bool scale_mode_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings) {
    UNUSED_PARAMETER(p);
    int mode = static_cast<int>(obs_data_get_int(settings, S_SCALE_MODE));
    obs_property_set_visible(obs_properties_get(props, S_SCALE_MULTIPLIER), mode == 0);
    obs_property_set_visible(obs_properties_get(props, S_TARGET_RES), mode == 1);
    return true;
}

static obs_properties_t *vsr_get_properties(void *data) {
    auto *filter = static_cast<vsr_filter_data *>(data);
    obs_properties_t *props = obs_properties_create();

    obs_property_t *mode = obs_properties_add_list(props, S_SCALE_MODE, obs_module_text("ScaleMode"),
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, obs_module_text("ScaleMultiplier"), 0);
    obs_property_list_add_int(mode, obs_module_text("TargetResolution"), 1);
    obs_property_set_modified_callback(mode, scale_mode_changed);

    obs_property_t *mult = obs_properties_add_float_slider(props, S_SCALE_MULTIPLIER, obs_module_text("ScaleMultiplier"), 1.0, 4.0, 0.25);

    obs_property_t *res = obs_properties_add_list(props, S_TARGET_RES, obs_module_text("TargetResolution"),
                                                 OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(res, "1280x720 (720p)", "1280x720");
    obs_property_list_add_string(res, "1920x1080 (1080p)", "1920x1080");
    obs_property_list_add_string(res, "2560x1440 (1440p)", "2560x1440");
    obs_property_list_add_string(res, "3840x2160 (4K UHD)", "3840x2160");

    obs_property_t *quality = obs_properties_add_list(props, S_QUALITY, obs_module_text("QualityPreset"),
                                                     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(quality, "Ultra (Level 4, Max Quality)", 4);
    obs_property_list_add_int(quality, "High (Level 3)", 3);
    obs_property_list_add_int(quality, "Medium (Level 2)", 2);
    obs_property_list_add_int(quality, "Low (Level 1, Fastest)", 1);
    obs_property_list_add_int(quality, "Denoise Low (1x Cleanup)", 8);
    obs_property_list_add_int(quality, "Denoise Medium (1x Cleanup)", 9);
    obs_property_list_add_int(quality, "Deblur Low (1x Sharpen)", 12);
    obs_property_list_add_int(quality, "Deblur Medium (1x Sharpen)", 13);
    obs_property_list_add_int(quality, "High-Bitrate Ultra", 19);

    if (filter) {
        update_status_text(filter);
    }
    obs_property_t *status = obs_properties_add_text(props, S_STATUS, obs_module_text("Status"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(status, false);

    return props;
}

struct obs_source_info vsr_filter_info = {
    .id = "rtx_vsr_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .get_name = vsr_get_name,
    .create = vsr_create,
    .destroy = vsr_destroy,
    .get_width = vsr_get_width,
    .get_height = vsr_get_height,
    .get_defaults = vsr_get_defaults,
    .get_properties = vsr_get_properties,
    .update = vsr_update,
    .video_tick = vsr_video_tick,
    .video_render = vsr_video_render,
    .video_get_color_space = vsr_get_color_space,
};
