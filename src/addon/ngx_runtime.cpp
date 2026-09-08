#include "ngx_runtime.hpp"

#include "dos2dlss/shared_state.hpp"

#include <windows.h>
#include <d3d11.h>

#include <cstdarg>
#include <cstdio>

#include "nvsdk_ngx_helpers.h"

namespace
{
constexpr unsigned long long kApplicationId = 0x1000000ULL;

void write_log(dos2dlss::NgxRuntime::LogFn log, const char *format, ...)
{
    if (log == nullptr)
        return;
    char message[384] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    log(message);
}

NVSDK_NGX_PerfQuality_Value perf_quality_for_mode(int mode)
{
    switch (mode)
    {
    case 1: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case 2: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 3: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case 4: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

const char *preset_parameter_for_mode(int mode)
{
    switch (mode)
    {
    case 1: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
    case 2: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    case 3: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
    case 4: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
    default: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    }
}
}

namespace dos2dlss
{
bool NgxRuntime::initialize(ID3D11Device *device, const wchar_t *application_data_path,
                            SharedState *state, LogFn log)
{
    if (initialized_)
        return device == device_;
    if (device == nullptr || application_data_path == nullptr || state == nullptr)
        return false;

    CreateDirectoryW(application_data_path, nullptr);
    const NVSDK_NGX_Result init_result = NVSDK_NGX_D3D11_Init(
        kApplicationId, application_data_path, device);
    InterlockedExchange(&state->ngx_init_result, static_cast<LONG>(init_result));
    if (NVSDK_NGX_FAILED(init_result))
    {
        write_log(log, "NGX D3D11 initialization failed: 0x%08lX.", init_result);
        return false;
    }

    device_ = device;
    device_->AddRef();
    initialized_ = true;
    InterlockedExchange(&state->ngx_initialized, 1);
    write_log(log, "NGX D3D11 initialized: 0x%08lX.", init_result);

    const NVSDK_NGX_Result caps_result =
        NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilities_);
    InterlockedExchange(&state->ngx_capability_result, static_cast<LONG>(caps_result));
    if (NVSDK_NGX_FAILED(caps_result) || capabilities_ == nullptr)
    {
        write_log(log, "NGX capability query failed: 0x%08lX.", caps_result);
        return true;
    }

    int available = 0;
    const NVSDK_NGX_Result available_result = NVSDK_NGX_Parameter_GetI(
        capabilities_, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (NVSDK_NGX_FAILED(available_result))
        write_log(log, "DLSS availability query failed: 0x%08lX.", available_result);
    InterlockedExchange(&state->ngx_available,
                        NVSDK_NGX_SUCCEED(available_result) && available != 0 ? 1 : 0);
    write_log(log, "NGX capability result 0x%08lX; DLSS available=%lu.",
              caps_result, available != 0 ? 1UL : 0UL);
    return true;
}

void NgxRuntime::release_feature(SharedState *state, LogFn log)
{
    if (feature_ != nullptr)
    {
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_ReleaseFeature(feature_);
        write_log(log, "NGX DLSS feature released: 0x%08lX.", result);
        feature_ = nullptr;
    }
    if (parameters_ != nullptr)
    {
        NVSDK_NGX_D3D11_DestroyParameters(parameters_);
        parameters_ = nullptr;
    }
    feature_mode_ = 0;
    output_width_ = 0;
    output_height_ = 0;
    if (state != nullptr)
    {
        InterlockedExchange(&state->ngx_feature_created, 0);
        InterlockedExchange(&state->render_width, 0);
        InterlockedExchange(&state->render_height, 0);
    }
}

void NgxRuntime::sync_feature(ID3D11DeviceContext *context, int mode,
                              std::uint32_t output_width, std::uint32_t output_height,
                              SharedState *state, LogFn log)
{
    if (!initialized_ || context == nullptr || state == nullptr)
        return;
    if (mode <= 0 || state->ngx_available == 0)
    {
        release_feature(state, log);
        return;
    }
    if (feature_ != nullptr && feature_mode_ == mode &&
        output_width_ == output_width && output_height_ == output_height)
        return;

    release_feature(state, log);
    if (output_width == 0 || output_height == 0)
        return;

    unsigned int render_width = output_width;
    unsigned int render_height = output_height;
    unsigned int max_width = output_width;
    unsigned int max_height = output_height;
    unsigned int min_width = output_width;
    unsigned int min_height = output_height;
    float sharpness = 0.0f;
    const NVSDK_NGX_PerfQuality_Value perf_quality = perf_quality_for_mode(mode);
    if (mode != 1 && capabilities_ != nullptr)
    {
        const NVSDK_NGX_Result optimal_result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            capabilities_, output_width, output_height, perf_quality,
            &render_width, &render_height, &max_width, &max_height,
            &min_width, &min_height, &sharpness);
        if (NVSDK_NGX_FAILED(optimal_result))
        {
            write_log(log, "NGX optimal-size query failed: 0x%08lX.", optimal_result);
            return;
        }
    }

    const NVSDK_NGX_Result allocate_result =
        NVSDK_NGX_D3D11_AllocateParameters(&parameters_);
    if (NVSDK_NGX_FAILED(allocate_result) || parameters_ == nullptr)
    {
        InterlockedExchange(&state->ngx_feature_result, static_cast<LONG>(allocate_result));
        write_log(log, "NGX parameter allocation failed: 0x%08lX.", allocate_result);
        return;
    }

    NVSDK_NGX_Parameter_SetI(parameters_, preset_parameter_for_mode(mode),
                            NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_DLSS_Create_Params create = {};
    create.Feature.InWidth = render_width;
    create.Feature.InHeight = render_height;
    create.Feature.InTargetWidth = output_width;
    create.Feature.InTargetHeight = output_height;
    create.Feature.InPerfQualityValue = perf_quality;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    create.InEnableOutputSubrects = false;

    const NVSDK_NGX_Result create_result =
        NGX_D3D11_CREATE_DLSS_EXT(context, &feature_, parameters_, &create);
    InterlockedExchange(&state->ngx_feature_result, static_cast<LONG>(create_result));
    if (NVSDK_NGX_FAILED(create_result) || feature_ == nullptr)
    {
        feature_ = nullptr;
        write_log(log, "NGX DLSS feature creation failed: 0x%08lX.", create_result);
        return;
    }

    feature_mode_ = mode;
    output_width_ = output_width;
    output_height_ = output_height;
    InterlockedExchange(&state->ngx_feature_created, 1);
    InterlockedExchange(&state->render_width, static_cast<LONG>(render_width));
    InterlockedExchange(&state->render_height, static_cast<LONG>(render_height));
    write_log(log, "NGX DLSS feature created: result=0x%08lX, input=%lux%lu, output=%lux%lu.",
              create_result, render_width, render_height, output_width, output_height);
}

bool NgxRuntime::evaluate(ID3D11DeviceContext *context, ID3D11Resource *color,
                          ID3D11Resource *output, ID3D11Resource *depth,
                          ID3D11Resource *motion_vectors, std::uint32_t render_width,
                          std::uint32_t render_height, bool reset,
                          SharedState *state, LogFn log)
{
    if (context == nullptr || feature_ == nullptr || parameters_ == nullptr ||
        color == nullptr || output == nullptr || depth == nullptr ||
        motion_vectors == nullptr || state == nullptr)
        return false;

    NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
    eval.Feature.pInColor = color;
    eval.Feature.pInOutput = output;
    eval.Feature.InSharpness = 0.0f;
    eval.pInDepth = depth;
    eval.pInMotionVectors = motion_vectors;
    eval.InJitterOffsetX = 0.0f;
    eval.InJitterOffsetY = 0.0f;
    eval.InRenderSubrectDimensions.Width = render_width;
    eval.InRenderSubrectDimensions.Height = render_height;
    eval.InReset = reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;

    const NVSDK_NGX_Result result =
        NGX_D3D11_EVALUATE_DLSS_EXT(context, feature_, parameters_, &eval);
    InterlockedExchange(&state->ngx_evaluate_result, static_cast<LONG>(result));
    if (NVSDK_NGX_FAILED(result))
    {
        write_log(log, "NGX DLSS evaluate failed: 0x%08lX.", result);
        return false;
    }
    InterlockedIncrement64(&state->ngx_evaluated_frames);
    return true;
}

void NgxRuntime::shutdown(SharedState *state, LogFn log)
{
    release_feature(state, log);
    if (capabilities_ != nullptr)
    {
        NVSDK_NGX_D3D11_DestroyParameters(capabilities_);
        capabilities_ = nullptr;
    }
    if (initialized_ && device_ != nullptr)
    {
        const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Shutdown1(device_);
        write_log(log, "NGX D3D11 shutdown: 0x%08lX.", result);
        device_->Release();
    }
    device_ = nullptr;
    initialized_ = false;
    if (state != nullptr)
    {
        InterlockedExchange(&state->ngx_initialized, 0);
        InterlockedExchange(&state->ngx_available, 0);
    }
}
}
