#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes */
typedef enum NvCV_Status {
    NVCV_SUCCESS = 0,
    NVCV_ERR_GENERAL = -1,
    NVCV_ERR_UNIMPLEMENTED = -2,
    NVCV_ERR_MEMORY = -3,
    NVCV_ERR_EFFECT = -4,
    NVCV_ERR_SELECTOR = -5,
    NVCV_ERR_BUFFER = -6,
    NVCV_ERR_PARAMETER = -7,
    NVCV_ERR_MISMATCH = -8,
    NVCV_ERR_PIXELFORMAT = -9,
    NVCV_ERR_MODEL = -10,
    NVCV_ERR_LIBRARY = -11,
    NVCV_ERR_INITIALIZATION = -12,
    NVCV_ERR_FILE = -13,
    NVCV_ERR_FEATURENOTFOUND = -14,
    NVCV_ERR_MISSINGINPUT = -15,
    NVCV_ERR_RESOLUTION = -16,
    NVCV_ERR_UNSUPPORTEDGPU = -17,
    NVCV_ERR_WRONGGPU = -18,
    NVCV_ERR_UNSUPPORTEDDRIVER = -19,
    NVCV_ERR_CUDA = -1099,
} NvCV_Status;

/* Pixel format */
typedef enum NvCVImage_PixelFormat {
    NVCV_FORMAT_UNKNOWN = 0,
    NVCV_Y = 1,
    NVCV_A = 2,
    NVCV_YA = 3,
    NVCV_RGB = 4,
    NVCV_BGR = 5,
    NVCV_RGBA = 6,
    NVCV_BGRA = 7,
    NVCV_ARGB = 8,
    NVCV_ABGR = 9,
    NVCV_YUV420 = 10,
    NVCV_YUV422 = 11,
    NVCV_YUV444 = 12,
    NVCV_RGB10A2 = 13,
} NvCVImage_PixelFormat;

/* Component type */
typedef enum NvCVImage_ComponentType {
    NVCV_TYPE_UNKNOWN = 0,
    NVCV_U8 = 1,
    NVCV_U16 = 2,
    NVCV_S16 = 3,
    NVCV_F16 = 4,
    NVCV_U32 = 5,
    NVCV_S32 = 6,
    NVCV_F32 = 7,
    NVCV_U64 = 8,
    NVCV_S64 = 9,
    NVCV_F64 = 10,
    NVCV_P32 = 11, // Packed 32-bit (e.g. RGB10A2)
} NvCVImage_ComponentType;

/* Memory layout and location flags */
#define NVCV_CHUNKY      0
#define NVCV_INTERLEAVED 0
#define NVCV_PLANAR      1

#define NVCV_CPU         0
#define NVCV_GPU         1
#define NVCV_CUDA        1
#define NVCV_CPU_PINNED  2
#define NVCV_CUDA_ARRAY  3

/* Effect Selector Strings */
#define NVVFX_FX_VIDEO_SUPER_RES        "VideoSuperRes"
#define NVVFX_FX_TRUE_HDR               "TrueHDR"
#define NVVFX_FX_VIDEO_FRAME_GENERATION "VideoFrameGeneration"

/* Parameter Selector Strings */
#define NVVFX_PARAM_SRC_IMAGE_0        "SrcImage0"
#define NVVFX_PARAM_SRC_IMAGE_1        "SrcImage1"
#define NVVFX_PARAM_DST_IMAGE_0        "DstImage0"
#define NVVFX_PARAM_MODEL_DIR          "ModelDir"
#define NVVFX_PARAM_CUDA_STREAM        "CudaStream"
#define NVVFX_PARAM_STRENGTH           "Strength"
#define NVVFX_PARAM_QUALITY_LEVEL      "QualityLevel"
#define NVVFX_PARAM_IMAGE_ENCODING     "ImageEncodingMode"

/* VideoFrameGeneration specific parameters */
#define NVVFX_PARAM_INPUT_WIDTH        "InputWidth"
#define NVVFX_PARAM_INPUT_HEIGHT       "InputHeight"
#define NVVFX_PARAM_MODE               "Mode"
#define NVVFX_PARAM_FRAME_MULTIPLIER   "FrameMultiplier"
#define NVVFX_PARAM_FRAME_INDEX        "FrameIndex"
#define NVVFX_PARAM_TIMESTEP           "Timestep"
#define NVVFX_PARAM_SHOT_CHANGE        "ShotChange"
#define NVVFX_PARAM_AUTO_SHOT_CHANGE   "AutomaticShotChangeDetectionEnabled"

/* TrueHDR specific parameters */
#define NVVFX_PARAM_CONTRAST           "Contrast"
#define NVVFX_PARAM_SATURATION         "Saturation"
#define NVVFX_PARAM_MIDDLE_GRAY        "MiddleGray"
#define NVVFX_PARAM_LUMINANCE          "Luminance"
#define NVVFX_PARAM_DEBANDING_OFF      "DebandingOff"

/* NvCVImage structure */
typedef struct NvCVImage {
    unsigned int width;
    unsigned int height;
    signed int pitch;
    NvCVImage_PixelFormat pixelFormat;
    NvCVImage_ComponentType componentType;
    unsigned char pixelBytes;
    unsigned char componentBytes;
    unsigned char numComponents;
    unsigned char planar;
    unsigned char gpuMem;
    unsigned char colorspace;
    unsigned char reserved[2];
    void *pixels;
    void *deletePtr;
    void (*deleteProc)(void *p);
    unsigned long long bufferBytes;
} NvCVImage;

typedef void *NvVFX_Handle;
typedef struct CUstream_st *CUstream;
typedef const char *NvVFX_EffectSelector;
typedef const char *NvVFX_ParameterSelector;

/* Function pointer types */
typedef NvCV_Status (*NvVFX_GetVersion_t)(unsigned int *version);
typedef NvCV_Status (*NvVFX_CreateEffect_t)(NvVFX_EffectSelector code, NvVFX_Handle *effect);
typedef NvCV_Status (*NvVFX_DestroyEffect_t)(NvVFX_Handle effect);
typedef NvCV_Status (*NvVFX_SetU32_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, unsigned int val);
typedef NvCV_Status (*NvVFX_SetS32_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, int val);
typedef NvCV_Status (*NvVFX_SetF32_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, float val);
typedef NvCV_Status (*NvVFX_SetString_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, const char *str);
typedef NvCV_Status (*NvVFX_SetImage_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, NvCVImage *im);
typedef NvCV_Status (*NvVFX_SetCudaStream_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, CUstream stream);

typedef NvCV_Status (*NvVFX_GetU32_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, unsigned int *val);
typedef NvCV_Status (*NvVFX_GetF32_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, float *val);
typedef NvCV_Status (*NvVFX_GetString_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, const char **str);
typedef NvCV_Status (*NvVFX_GetImage_t)(NvVFX_Handle effect, NvVFX_ParameterSelector param, NvCVImage *im);

typedef NvCV_Status (*NvVFX_Load_t)(NvVFX_Handle effect);
typedef NvCV_Status (*NvVFX_Run_t)(NvVFX_Handle effect, int async);
typedef NvCV_Status (*NvVFX_CudaStreamCreate_t)(CUstream *stream);
typedef NvCV_Status (*NvVFX_CudaStreamDestroy_t)(CUstream stream);

typedef const char *(*NvCV_GetErrorStringFromCode_t)(NvCV_Status code);
typedef NvCV_Status (*NvCVImage_Init_t)(NvCVImage *im, unsigned width, unsigned height, int pitch,
                                      void *pixels, NvCVImage_PixelFormat format,
                                      NvCVImage_ComponentType type, unsigned layout, unsigned memSpace);
typedef NvCV_Status (*NvCVImage_Alloc_t)(NvCVImage *im, unsigned width, unsigned height,
                                       NvCVImage_PixelFormat format, NvCVImage_ComponentType type,
                                       unsigned layout, unsigned memSpace, unsigned alignment);
typedef NvCV_Status (*NvCVImage_Dealloc_t)(NvCVImage *im);
typedef NvCV_Status (*NvCVImage_Transfer_t)(const NvCVImage *src, NvCVImage *dst, float scale,
                                          CUstream stream, NvCVImage *tmp);

#ifdef __cplusplus
}
#endif
