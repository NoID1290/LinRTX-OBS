#include "nvvfx-loader.h"
#include "cuda-gl-interop.h"
#include "kernels/format_convert.h"
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <chrono>
#include <string>
#include <cmath>

#define S_QUALITY_MODE      "quality_mode"
#define S_CADENCE_MODE      "cadence_mode"
#define S_MULTIPLIER        "multiplier"
#define S_TIMESTEP          "timestep"
#define S_AUTO_SHOT_CHANGE  "auto_shot_change"
#define S_DIFF_SENSITIVITY  "diff_sensitivity"
#define S_STATUS            "status_info"

enum FrameGenQuality {
    QUALITY_LOW = 0,
    QUALITY_MEDIUM = 1,
    QUALITY_HIGH = 2
};

enum FrameGenCadence {
    CADENCE_AUTO = 0,         // Auto-Cadence Motion Smoothing
    CADENCE_INTERLEAVED = 1,   // Strict 2x Alternating
    CADENCE_MULTIPLIER = 2,    // Fixed Multiplier (2x, 3x, 4x)
    CADENCE_MANUAL = 3         // Manual Timestep
};

struct framegen_filter_data {
    obs_source_t *context = nullptr;
    gs_effect_t *effect = nullptr;
    gs_eparam_t *image_param = nullptr;
    gs_eparam_t *multiplier_param = nullptr;

    // Filter settings
    int quality_mode = QUALITY_MEDIUM;
    int cadence_mode = CADENCE_AUTO;
    int multiplier = 2;
    float timestep = 0.5f;
    bool auto_shot_change = true;
    int diff_sensitivity = 10; // Threshold = diff_sensitivity * 1000

    // Source Dimensions
    uint32_t width = 0;
    uint32_t height = 0;

    // Graphics & CUDA interop
    gs_texrender_t *texrender_in = nullptr;
    gs_texture_t *texture_out = nullptr;
    linrtx::CudaGLTextureBridge bridge_in;
    linrtx::CudaGLTextureBridge bridge_out;

    // Pitched linear CUDA buffers
    linrtx::PitchedBuffer buf_in;    // Newly rendered frame from OBS
    linrtx::PitchedBuffer buf_prev;  // Frame N-1 (SrcImage0)
    linrtx::PitchedBuffer buf_curr;  // Frame N   (SrcImage1)
    linrtx::PitchedBuffer buf_gen;   // Generated intermediate frame (DstImage0)

    // Device and host diff accumulators for fast frame difference checking
    unsigned long long *d_diff_accum = nullptr;
    unsigned long long *h_diff_accum = nullptr;

    // VFX SDK state
    NvVFX_Handle vfx_handle = nullptr;
    CUstream stream = nullptr;
    NvCVImage src0_img = {};
    NvCVImage src1_img = {};
    NvCVImage dst_img = {};
    bool vfx_initialized = false;
    bool needs_reload = false;

    // Frame interpolation state tracking
    bool has_prev = false;
    bool has_curr = false;
    bool has_gen = false;
    int consecutive_repeats = 0;
    uint64_t total_render_ticks = 0;
    uint64_t total_source_frames = 0;

    // Performance telemetry
    float last_ai_ms = 0.0f;
    float last_ai_fps = 0.0f;
    float detected_src_fps = 0.0f;
    float canvas_fps = 0.0f;
    std::chrono::steady_clock::time_point last_stats_time;
    uint64_t stats_tick_count = 0;
    uint64_t stats_src_count = 0;
    std::string status_msg;
};

static const char *framegen_get_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return obs_module_text("RTXVideoFrameGeneration");
}

static void update_status_text(framegen_filter_data *filter) {
    if (!linrtx::is_sdk_loaded()) {
        filter->status_msg = "SDK Status: NOT FOUND (" + linrtx::get_sdk_error() + ")";
        return;
    }
    if (!filter->vfx_initialized) {
        filter->status_msg = "SDK: Ready | Model: Initializing...";
        return;
    }

    const char *q_str = "Medium";
    if (filter->quality_mode == QUALITY_LOW) q_str = "Low";
    else if (filter->quality_mode == QUALITY_HIGH) q_str = "High";

    char buf[256];
    snprintf(buf, sizeof(buf),
             "SDK: Ready | %ux%u (%s) | AI: %.2f ms (%.0f FPS) | Source: %.1f FPS -> Canvas: %.1f FPS",
             filter->width, filter->height, q_str,
             filter->last_ai_ms, filter->last_ai_fps,
             filter->detected_src_fps, filter->canvas_fps);
    filter->status_msg = buf;
}

static void framegen_destroy_vfx(framegen_filter_data *filter) {
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
    filter->buf_prev.free();
    filter->buf_curr.free();
    filter->buf_gen.free();

    if (filter->d_diff_accum) {
        cudaFree(filter->d_diff_accum);
        filter->d_diff_accum = nullptr;
    }
    if (filter->h_diff_accum) {
        cudaFreeHost(filter->h_diff_accum);
        filter->h_diff_accum = nullptr;
    }

    filter->has_prev = false;
    filter->has_curr = false;
    filter->has_gen = false;
    filter->consecutive_repeats = 0;
    filter->vfx_initialized = false;
}

static bool framegen_init_vfx(framegen_filter_data *filter) {
    if (!linrtx::is_sdk_loaded()) {
        return false;
    }
    if (filter->width == 0 || filter->height == 0) {
        return false;
    }

    const auto &api = linrtx::vfx();

    if (!filter->stream) {
        NvCV_Status err = api.CudaStreamCreate(&filter->stream);
        if (err != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[LinRTX FG] Failed to create CUDA stream: %s", linrtx::status_to_string(err).c_str());
            return false;
        }
    }

    // Allocate diff accumulators
    if (!filter->d_diff_accum) {
        cudaError_t cu_err = cudaMalloc(&filter->d_diff_accum, sizeof(unsigned long long));
        if (cu_err != cudaSuccess) {
            blog(LOG_ERROR, "[LinRTX FG] cudaMalloc diff_accum failed: %s", cudaGetErrorString(cu_err));
            return false;
        }
    }
    if (!filter->h_diff_accum) {
        cudaError_t cu_err = cudaMallocHost(&filter->h_diff_accum, sizeof(unsigned long long));
        if (cu_err != cudaSuccess) {
            blog(LOG_ERROR, "[LinRTX FG] cudaMallocHost diff_accum failed: %s", cudaGetErrorString(cu_err));
            return false;
        }
    }

    // Allocate pitched linear buffers for all frame stages
    size_t row_bytes = filter->width * 4;
    if (!filter->buf_in.allocate(row_bytes, filter->height) ||
        !filter->buf_prev.allocate(row_bytes, filter->height) ||
        !filter->buf_curr.allocate(row_bytes, filter->height) ||
        !filter->buf_gen.allocate(row_bytes, filter->height)) {
        blog(LOG_ERROR, "[LinRTX FG] Failed to allocate pitched buffers for %ux%u", filter->width, filter->height);
        return false;
    }

    // Initialize NvCVImage descriptors
    api.ImageInit(&filter->src0_img, filter->width, filter->height,
                  static_cast<int>(filter->buf_prev.pitch), filter->buf_prev.ptr,
                  NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA);

    api.ImageInit(&filter->src1_img, filter->width, filter->height,
                  static_cast<int>(filter->buf_curr.pitch), filter->buf_curr.ptr,
                  NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA);

    api.ImageInit(&filter->dst_img, filter->width, filter->height,
                  static_cast<int>(filter->buf_gen.pitch), filter->buf_gen.ptr,
                  NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CUDA);

    // Create VideoFrameGeneration effect
    if (!filter->vfx_handle) {
        NvCV_Status err = api.CreateEffect(NVVFX_FX_VIDEO_FRAME_GENERATION, &filter->vfx_handle);
        if (err != NVCV_SUCCESS) {
            blog(LOG_ERROR, "[LinRTX FG] Failed to create VideoFrameGeneration effect: %s", linrtx::status_to_string(err).c_str());
            return false;
        }
    }

    api.SetCudaStream(filter->vfx_handle, NVVFX_PARAM_CUDA_STREAM, filter->stream);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_INPUT_WIDTH, filter->width);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_INPUT_HEIGHT, filter->height);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_MODE, static_cast<unsigned int>(filter->quality_mode));
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_IMAGE_ENCODING, 0); // 8-bit RGBA
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_AUTO_SHOT_CHANGE, filter->auto_shot_change ? 1 : 0);
    api.SetU32(filter->vfx_handle, NVVFX_PARAM_SHOT_CHANGE, 0);

    api.SetImage(filter->vfx_handle, NVVFX_PARAM_SRC_IMAGE_0, &filter->src0_img);
    api.SetImage(filter->vfx_handle, NVVFX_PARAM_SRC_IMAGE_1, &filter->src1_img);
    api.SetImage(filter->vfx_handle, NVVFX_PARAM_DST_IMAGE_0, &filter->dst_img);

    if (filter->cadence_mode == CADENCE_MANUAL) {
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_FRAME_MULTIPLIER, 0);
        api.SetF32(filter->vfx_handle, NVVFX_PARAM_TIMESTEP, filter->timestep);
    } else {
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_FRAME_MULTIPLIER, static_cast<unsigned int>(filter->multiplier));
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_FRAME_INDEX, 1);
    }

    blog(LOG_INFO, "[LinRTX FG] Loading VideoFrameGeneration model (%ux%u, mode %d)...",
         filter->width, filter->height, filter->quality_mode);

    auto t0 = std::chrono::high_resolution_clock::now();
    NvCV_Status err = api.Load(filter->vfx_handle);
    auto t1 = std::chrono::high_resolution_clock::now();
    if (err != NVCV_SUCCESS) {
        blog(LOG_ERROR, "[LinRTX FG] NvVFX_Load failed: %s", linrtx::status_to_string(err).c_str());
        return false;
    }
    double load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    blog(LOG_INFO, "[LinRTX FG] VideoFrameGeneration model loaded successfully in %.2f ms", load_ms);

    filter->vfx_initialized = true;
    filter->needs_reload = false;
    filter->has_prev = false;
    filter->has_curr = false;
    filter->has_gen = false;
    filter->consecutive_repeats = 0;
    return true;
}

static void framegen_update(void *data, obs_data_t *settings) {
    auto *filter = static_cast<framegen_filter_data *>(data);

    int new_q = static_cast<int>(obs_data_get_int(settings, S_QUALITY_MODE));
    int new_c = static_cast<int>(obs_data_get_int(settings, S_CADENCE_MODE));
    int new_m = static_cast<int>(obs_data_get_int(settings, S_MULTIPLIER));
    float new_t = static_cast<float>(obs_data_get_double(settings, S_TIMESTEP));
    bool new_sc = obs_data_get_bool(settings, S_AUTO_SHOT_CHANGE);
    int new_sens = static_cast<int>(obs_data_get_int(settings, S_DIFF_SENSITIVITY));

    if (new_m < 2) new_m = 2;
    if (new_m > 8) new_m = 8;
    if (new_t <= 0.0f) new_t = 0.05f;
    if (new_t >= 1.0f) new_t = 0.95f;

    bool reload_needed = (new_q != filter->quality_mode);

    filter->quality_mode = new_q;
    filter->cadence_mode = new_c;
    filter->multiplier = new_m;
    filter->timestep = new_t;
    filter->auto_shot_change = new_sc;
    filter->diff_sensitivity = new_sens;

    if (filter->vfx_initialized && !reload_needed) {
        const auto &api = linrtx::vfx();
        api.SetU32(filter->vfx_handle, NVVFX_PARAM_AUTO_SHOT_CHANGE, filter->auto_shot_change ? 1 : 0);
        if (filter->cadence_mode == CADENCE_MANUAL) {
            api.SetU32(filter->vfx_handle, NVVFX_PARAM_FRAME_MULTIPLIER, 0);
            api.SetF32(filter->vfx_handle, NVVFX_PARAM_TIMESTEP, filter->timestep);
        } else {
            api.SetU32(filter->vfx_handle, NVVFX_PARAM_FRAME_MULTIPLIER, static_cast<unsigned int>(filter->multiplier));
            api.SetU32(filter->vfx_handle, NVVFX_PARAM_FRAME_INDEX, 1);
        }
    } else if (reload_needed) {
        filter->needs_reload = true;
    }
}

static void *framegen_create(obs_data_t *settings, obs_source_t *context) {
    auto *filter = new framegen_filter_data();
    filter->context = context;

    obs_enter_graphics();
    filter->effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    filter->image_param = gs_effect_get_param_by_name(filter->effect, "image");
    filter->multiplier_param = gs_effect_get_param_by_name(filter->effect, "multiplier");
    obs_leave_graphics();

    linrtx::load_sdk();
    framegen_update(filter, settings);

    filter->last_stats_time = std::chrono::steady_clock::now();
    return filter;
}

static void framegen_destroy(void *data) {
    auto *filter = static_cast<framegen_filter_data *>(data);
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

    framegen_destroy_vfx(filter);
    delete filter;
}

static void framegen_video_tick(void *data, float seconds) {
    UNUSED_PARAMETER(seconds);
    auto *filter = static_cast<framegen_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) return;

    uint32_t cur_w = obs_source_get_base_width(target);
    uint32_t cur_h = obs_source_get_base_height(target);
    if (cur_w == 0 || cur_h == 0) return;

    // 4-pixel alignment for optimal GPU pitch
    cur_w = (cur_w + 3) & ~3;
    cur_h = (cur_h + 3) & ~3;

    if (cur_w != filter->width || cur_h != filter->height || filter->needs_reload) {
        filter->width = cur_w;
        filter->height = cur_h;
        filter->needs_reload = true;
    }

    // Telemetry updates once per second
    filter->stats_tick_count++;
    auto now = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(now - filter->last_stats_time).count();
    if (elapsed_sec >= 1.0) {
        filter->canvas_fps = static_cast<float>(filter->stats_tick_count / elapsed_sec);
        filter->detected_src_fps = static_cast<float>(filter->stats_src_count / elapsed_sec);
        filter->stats_tick_count = 0;
        filter->stats_src_count = 0;
        filter->last_stats_time = now;
        update_status_text(filter);
    }
}

static void framegen_video_render(void *data, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);
    auto *filter = static_cast<framegen_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);
    obs_source_t *parent = obs_filter_get_parent(filter->context);

    if (!target || !parent || filter->width == 0 || filter->height == 0) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    if (!linrtx::is_sdk_loaded()) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Reallocate GL textures and VFX engine on size or config change
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
        filter->texture_out = gs_texture_create(filter->width, filter->height, GS_RGBA, 1, nullptr, GS_DYNAMIC);
        obs_leave_graphics();

        framegen_destroy_vfx(filter);
        if (!framegen_init_vfx(filter)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }

    // Step 1: Render upstream source into input gs_texrender
    uint32_t parent_flags = obs_source_get_output_flags(parent);
    bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
    bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;

    gs_texrender_reset(filter->texrender_in);
    if (!gs_texrender_begin(filter->texrender_in, filter->width, filter->height)) {
        obs_source_skip_video_filter(filter->context);
        return;
    }
    struct vec4 clear_color = {};
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f, static_cast<float>(filter->width), 0.0f, static_cast<float>(filter->height), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    if (target == parent && !custom_draw && !async) {
        obs_source_default_render(target);
    } else {
        obs_source_video_render(target);
    }

    gs_blend_state_pop();
    gs_texrender_end(filter->texrender_in);

    gs_texture_t *in_tex = gs_texrender_get_texture(filter->texrender_in);
    if (!in_tex || !filter->texture_out) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Register textures with CUDA for zero-copy VRAM interop
    if (!filter->bridge_in.is_registered() || filter->bridge_in.gs_texture() != in_tex) {
        if (!filter->bridge_in.register_texture(in_tex, filter->width, filter->height, cudaGraphicsRegisterFlagsReadOnly)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
    }
    if (!filter->bridge_out.is_registered() || filter->bridge_out.gs_texture() != filter->texture_out) {
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
        filter->total_render_ticks++;

        // Step 3: Copy input GL texture array -> linear CUDA buffer
        cudaMemcpy2DFromArrayAsync(
            filter->buf_in.ptr, filter->buf_in.pitch,
            arr_in, 0, 0,
            filter->width * 4, filter->height,
            cudaMemcpyDeviceToDevice, stream
        );

        bool is_new_frame = false;

        if (!filter->has_curr) {
            // First frame received: populate both prev and curr with this frame
            cudaMemcpy2DAsync(
                filter->buf_curr.ptr, filter->buf_curr.pitch,
                filter->buf_in.ptr, filter->buf_in.pitch,
                filter->width * 4, filter->height,
                cudaMemcpyDeviceToDevice, stream
            );
            cudaMemcpy2DAsync(
                filter->buf_prev.ptr, filter->buf_prev.pitch,
                filter->buf_in.ptr, filter->buf_in.pitch,
                filter->width * 4, filter->height,
                cudaMemcpyDeviceToDevice, stream
            );
            filter->has_curr = true;
            filter->has_prev = true;
            is_new_frame = true;
            filter->stats_src_count++;
        } else {
            // Fast strided difference check against current stored frame
            cudaMemsetAsync(filter->d_diff_accum, 0, sizeof(unsigned long long), stream);
            linrtx::launch_compute_frame_diff(
                filter->buf_in.ptr, filter->buf_in.pitch,
                filter->buf_curr.ptr, filter->buf_curr.pitch,
                filter->width, filter->height,
                filter->d_diff_accum, stream
            );
            cudaMemcpyAsync(
                filter->h_diff_accum, filter->d_diff_accum,
                sizeof(unsigned long long),
                cudaMemcpyDeviceToHost, stream
            );
            cudaStreamSynchronize(stream);

            unsigned long long diff_val = *filter->h_diff_accum;
            unsigned long long threshold = static_cast<unsigned long long>(filter->diff_sensitivity) * 1000ULL;

            if (diff_val > threshold) {
                is_new_frame = true;
                filter->stats_src_count++;
                filter->total_source_frames++;
            }
        }

        const auto &api = linrtx::vfx();

        // If a new source frame arrived, advance the frame pair and generate intermediate frame
        if (is_new_frame && filter->has_prev) {
            filter->consecutive_repeats = 0;

            // Shift: previous = current, current = new incoming
            cudaMemcpy2DAsync(
                filter->buf_prev.ptr, filter->buf_prev.pitch,
                filter->buf_curr.ptr, filter->buf_curr.pitch,
                filter->width * 4, filter->height,
                cudaMemcpyDeviceToDevice, stream
            );
            cudaMemcpy2DAsync(
                filter->buf_curr.ptr, filter->buf_curr.pitch,
                filter->buf_in.ptr, filter->buf_in.pitch,
                filter->width * 4, filter->height,
                cudaMemcpyDeviceToDevice, stream
            );

            // Execute AI Video Frame Generation
            auto t0 = std::chrono::high_resolution_clock::now();
            NvCV_Status run_err = api.Run(filter->vfx_handle, 0);
            cudaStreamSynchronize(stream);
            auto t1 = std::chrono::high_resolution_clock::now();

            if (run_err == NVCV_SUCCESS) {
                float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
                filter->last_ai_ms = ms;
                filter->last_ai_fps = (ms > 0.001f) ? (1000.0f / ms) : 0.0f;
                filter->has_gen = true;
            } else {
                blog(LOG_ERROR, "[LinRTX FG] NvVFX_Run error: %s", linrtx::status_to_string(run_err).c_str());
            }
        } else if (!is_new_frame) {
            filter->consecutive_repeats++;
        }

        // Step 4: Frame Cadence & Pacing Selection
        linrtx::PitchedBuffer *out_buf = &filter->buf_curr;

        if (filter->cadence_mode == CADENCE_AUTO) {
            // Smooth Cadence / Motion Smoothing Mode:
            // When a new frame arrives, display previous frame for perfect temporal pacing.
            // When a repeat tick arrives, display generated intermediate frame!
            if (is_new_frame) {
                out_buf = filter->has_prev ? &filter->buf_prev : &filter->buf_curr;
            } else {
                if (filter->consecutive_repeats == 1 && filter->has_gen) {
                    out_buf = &filter->buf_gen;
                } else {
                    out_buf = &filter->buf_curr;
                }
            }
        } else if (filter->cadence_mode == CADENCE_INTERLEAVED) {
            // Strict 2x Alternating: even tick = original frame, odd tick = generated frame
            if ((filter->total_render_ticks % 2 == 1) && filter->has_gen) {
                out_buf = &filter->buf_gen;
            } else {
                out_buf = &filter->buf_curr;
            }
        } else if (filter->cadence_mode == CADENCE_MULTIPLIER) {
            // Multiplier mode: cycles through sub-indices
            uint32_t cycle = filter->total_render_ticks % static_cast<uint32_t>(filter->multiplier);
            if (cycle == 0) {
                out_buf = &filter->buf_curr;
            } else if (filter->has_gen) {
                out_buf = &filter->buf_gen;
            }
        } else if (filter->cadence_mode == CADENCE_MANUAL) {
            // Always display generated frame at configured timestep
            out_buf = filter->has_gen ? &filter->buf_gen : &filter->buf_curr;
        }

        // Step 5: Copy selected buffer -> output GL texture array
        cudaMemcpy2DToArrayAsync(
            arr_out, 0, 0,
            out_buf->ptr, out_buf->pitch,
            filter->width * 4, filter->height,
            cudaMemcpyDeviceToDevice, stream
        );
    }

    filter->bridge_in.unmap(stream);
    filter->bridge_out.unmap(stream);

    // Step 6: Render output texture to OBS active target with full color space management
    enum gs_color_space current_space = gs_get_color_space();
    const char *technique = "Draw";

    if (current_space == GS_CS_709_SCRGB) {
        technique = "DrawSrgbDecompressMultiply";
        if (filter->multiplier_param) {
            float sdr_white = obs_get_video_sdr_white_level();
            gs_effect_set_float(filter->multiplier_param, sdr_white / 80.0f);
        }
    } else if (current_space == GS_CS_709_EXTENDED) {
        technique = "DrawSrgbDecompress";
    }

    gs_effect_set_texture(filter->image_param, filter->texture_out);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    while (gs_effect_loop(filter->effect, technique)) {
        gs_draw_sprite(filter->texture_out, 0, filter->width, filter->height);
    }

    gs_blend_state_pop();
}

static uint32_t framegen_get_width(void *data) {
    auto *filter = static_cast<framegen_filter_data *>(data);
    return filter->width;
}

static uint32_t framegen_get_height(void *data) {
    auto *filter = static_cast<framegen_filter_data *>(data);
    return filter->height;
}

static void framegen_get_defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, S_QUALITY_MODE, QUALITY_MEDIUM);
    obs_data_set_default_int(settings, S_CADENCE_MODE, CADENCE_AUTO);
    obs_data_set_default_int(settings, S_MULTIPLIER, 2);
    obs_data_set_default_double(settings, S_TIMESTEP, 0.50);
    obs_data_set_default_bool(settings, S_AUTO_SHOT_CHANGE, true);
    obs_data_set_default_int(settings, S_DIFF_SENSITIVITY, 10);
    obs_data_set_default_string(settings, S_STATUS, "SDK: Initializing...");
}

static bool cadence_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings) {
    UNUSED_PARAMETER(prop);
    int cadence = static_cast<int>(obs_data_get_int(settings, S_CADENCE_MODE));

    obs_property_t *p_mult = obs_properties_get(props, S_MULTIPLIER);
    obs_property_t *p_time = obs_properties_get(props, S_TIMESTEP);

    if (p_mult) obs_property_set_visible(p_mult, cadence == CADENCE_MULTIPLIER);
    if (p_time) obs_property_set_visible(p_time, cadence == CADENCE_MANUAL);

    return true;
}

static obs_properties_t *framegen_get_properties(void *data) {
    auto *filter = static_cast<framegen_filter_data *>(data);
    obs_properties_t *props = obs_properties_create();

    // Live Status readout
    obs_property_t *p_status = obs_properties_add_text(props, S_STATUS, obs_module_text("Status"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(p_status, false);

    // AI Quality Preset
    obs_property_t *p_quality = obs_properties_add_list(props, S_QUALITY_MODE,
                                                       obs_module_text("FrameGenMode"),
                                                       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p_quality, obs_module_text("FrameGenMode.Low"), QUALITY_LOW);
    obs_property_list_add_int(p_quality, obs_module_text("FrameGenMode.Medium"), QUALITY_MEDIUM);
    obs_property_list_add_int(p_quality, obs_module_text("FrameGenMode.High"), QUALITY_HIGH);

    // Cadence Mode
    obs_property_t *p_cadence = obs_properties_add_list(props, S_CADENCE_MODE,
                                                        obs_module_text("CadenceMode"),
                                                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p_cadence, obs_module_text("CadenceMode.Auto"), CADENCE_AUTO);
    obs_property_list_add_int(p_cadence, obs_module_text("CadenceMode.Interleaved"), CADENCE_INTERLEAVED);
    obs_property_list_add_int(p_cadence, obs_module_text("CadenceMode.Multiplier"), CADENCE_MULTIPLIER);
    obs_property_list_add_int(p_cadence, obs_module_text("CadenceMode.Manual"), CADENCE_MANUAL);
    obs_property_set_modified_callback(p_cadence, cadence_changed);

    // Multiplier (2x, 3x, 4x)
    obs_property_t *p_mult = obs_properties_add_list(props, S_MULTIPLIER,
                                                    obs_module_text("FrameMultiplier"),
                                                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p_mult, "2x (1 Generated Frame per Pair)", 2);
    obs_property_list_add_int(p_mult, "3x (2 Generated Frames per Pair)", 3);
    obs_property_list_add_int(p_mult, "4x (3 Generated Frames per Pair)", 4);

    // Timestep slider (0.05 - 0.95)
    obs_properties_add_float_slider(props, S_TIMESTEP, obs_module_text("FrameTimestep"), 0.05, 0.95, 0.05);

    // Automatic Shot Change Detection
    obs_properties_add_bool(props, S_AUTO_SHOT_CHANGE, obs_module_text("AutoShotChange"));

    // Sensitivity threshold slider
    obs_properties_add_int_slider(props, S_DIFF_SENSITIVITY, obs_module_text("DiffSensitivity"), 1, 50, 1);

    if (filter) {
        update_status_text(filter);
        obs_data_t *s = obs_source_get_settings(filter->context);
        if (s) {
            obs_data_set_string(s, S_STATUS, filter->status_msg.c_str());
            obs_data_release(s);
        }
    }

    return props;
}

static enum gs_color_space framegen_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces) {
    auto *filter = static_cast<framegen_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(filter->context);
    const enum gs_color_space potential_spaces[] = {
        GS_CS_SRGB,
        GS_CS_SRGB_16F,
        GS_CS_709_EXTENDED,
        GS_CS_709_SCRGB,
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

struct obs_source_info framegen_filter_info = {
    .id                    = "rtx_framegen_filter",
    .type                  = OBS_SOURCE_TYPE_FILTER,
    .output_flags          = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
    .get_name              = framegen_get_name,
    .create                = framegen_create,
    .destroy               = framegen_destroy,
    .get_width             = framegen_get_width,
    .get_height            = framegen_get_height,
    .get_defaults          = framegen_get_defaults,
    .get_properties        = framegen_get_properties,
    .update                = framegen_update,
    .video_tick            = framegen_video_tick,
    .video_render          = framegen_video_render,
    .video_get_color_space = framegen_get_color_space,
};
