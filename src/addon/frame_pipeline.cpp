#include "frame_pipeline.hpp"

#include "dos2dlss/shared_state.hpp"
#include "ngx_runtime.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace
{
constexpr char kCameraMotionShader[] = R"(
cbuffer MotionConstants : register(b0)
{
    row_major float4x4 CurrentInverseViewProjection;
    row_major float4x4 PreviousViewProjection;
    float2 RenderSize;
    float2 CurrentJitter;
};

Texture2D<float> SceneDepth : register(t0);
RWTexture2D<float2> MotionOutput : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)RenderSize.x || id.y >= (uint)RenderSize.y)
        return;
    float2 currentPixel = float2(id.xy) + 0.5;
    float2 currentUV = currentPixel / RenderSize;
    float2 unjitteredUV = currentUV - CurrentJitter / RenderSize;
    float depth = SceneDepth.Load(int3(id.xy, 0));
    float4 clip = float4(unjitteredUV.x * 2.0 - 1.0,
                         1.0 - unjitteredUV.y * 2.0, depth, 1.0);
    float4 world = mul(CurrentInverseViewProjection, clip);
    world /= world.w;
    float4 previousClip = mul(PreviousViewProjection, world);
    float2 previousUV = previousClip.xy / previousClip.w;
    previousUV = float2(previousUV.x * 0.5 + 0.5,
                        0.5 - previousUV.y * 0.5);
    MotionOutput[id.xy] = (previousUV - unjitteredUV) * RenderSize;
}
)";

void write_log(dos2dlss::FramePipeline::LogFn log, const char *format, ...)
{
    if (log == nullptr)
        return;
    char message[512] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    log(message);
}

template <class T>
void release(T *&object)
{
    if (object != nullptr)
    {
        object->Release();
        object = nullptr;
    }
}

struct MotionConstants
{
    DirectX::XMFLOAT4X4 current_inverse_view_projection;
    DirectX::XMFLOAT4X4 previous_view_projection;
    float render_size[2];
    float current_jitter[2];
};
static_assert(sizeof(MotionConstants) == 144);
}

namespace dos2dlss
{
bool FramePipeline::initialize(ID3D11Device *device, LogFn log)
{
    if (device_ != nullptr)
        return device_ == device;
    if (device == nullptr)
        return false;

    ID3DBlob *bytecode = nullptr;
    ID3DBlob *errors = nullptr;
    const HRESULT compile = D3DCompile(kCameraMotionShader, sizeof(kCameraMotionShader) - 1,
                                       "DOS2DLSS camera motion", nullptr, nullptr,
                                       "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                       0, &bytecode, &errors);
    if (FAILED(compile) || bytecode == nullptr)
    {
        write_log(log, "Camera-motion shader compilation failed: 0x%08lX%s%s", compile,
                  errors != nullptr ? ": " : "",
                  errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        release(errors);
        release(bytecode);
        return false;
    }
    const HRESULT created = device->CreateComputeShader(bytecode->GetBufferPointer(),
                                                         bytecode->GetBufferSize(), nullptr,
                                                         &motion_shader_);
    release(errors);
    release(bytecode);
    if (FAILED(created))
    {
        write_log(log, "Camera-motion shader creation failed: 0x%08lX", created);
        return false;
    }

    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = sizeof(MotionConstants);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&constants, nullptr, &motion_constants_)))
    {
        write_log(log, "Camera-motion constant buffer creation failed.");
        release(motion_shader_);
        return false;
    }
    device_ = device;
    device_->AddRef();
    write_log(log, "Camera-motion GPU pipeline initialized.");
    return true;
}

void FramePipeline::set_scene_depth(ID3D11Resource *depth)
{
    if (scene_depth_ == depth)
        return;
    release(depth_view_);
    release(scene_depth_);
    scene_depth_ = depth;
    if (scene_depth_ != nullptr)
        scene_depth_->AddRef();
}

void FramePipeline::reset_history()
{
    has_previous_camera_ = false;
}

bool FramePipeline::create_depth_view(LogFn log)
{
    if (depth_view_ != nullptr)
        return true;
    if (device_ == nullptr || scene_depth_ == nullptr)
        return false;
    ID3D11Texture2D *texture = nullptr;
    if (FAILED(scene_depth_->QueryInterface(IID_PPV_ARGS(&texture))))
        return false;
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture->GetDesc(&texture_desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.ViewDimension = texture_desc.SampleDesc.Count > 1
        ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    switch (texture_desc.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        view.Format = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        view.Format = DXGI_FORMAT_R16_UNORM;
        break;
    default:
        write_log(log, "Unsupported scene-depth format %u.", texture_desc.Format);
        texture->Release();
        return false;
    }
    const HRESULT result = device_->CreateShaderResourceView(texture, &view, &depth_view_);
    texture->Release();
    if (FAILED(result))
    {
        write_log(log, "Scene-depth SRV creation failed: 0x%08lX.", result);
        return false;
    }
    return true;
}

bool FramePipeline::ensure_resources(std::uint32_t render_width,
                                     std::uint32_t render_height,
                                     std::uint32_t output_width,
                                     std::uint32_t output_height, LogFn log)
{
    if (render_width_ == render_width && render_height_ == render_height &&
        output_width_ == output_width && output_height_ == output_height &&
        motion_texture_ != nullptr && output_texture_ != nullptr && create_depth_view(log))
        return true;
    release(motion_uav_);
    release(motion_texture_);
    release(compact_color_);
    compact_color_format_ = 0;
    release(output_view_);
    release(output_texture_);
    render_width_ = 0;
    render_height_ = 0;
    output_width_ = 0;
    output_height_ = 0;
    has_previous_camera_ = false;
    if (device_ == nullptr || render_width == 0 || render_height == 0 ||
        output_width == 0 || output_height == 0 || !create_depth_view(log))
        return false;

    D3D11_TEXTURE2D_DESC motion = {};
    motion.Width = render_width;
    motion.Height = render_height;
    motion.MipLevels = 1;
    motion.ArraySize = 1;
    motion.Format = DXGI_FORMAT_R16G16_FLOAT;
    motion.SampleDesc.Count = 1;
    motion.Usage = D3D11_USAGE_DEFAULT;
    motion.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D *motion_texture = nullptr;
    HRESULT result = device_->CreateTexture2D(&motion, nullptr, &motion_texture);
    if (SUCCEEDED(result))
        motion_texture_ = motion_texture;
    if (FAILED(result) || FAILED(device_->CreateUnorderedAccessView(
            motion_texture_, nullptr, &motion_uav_)))
    {
        write_log(log, "Motion texture creation failed: 0x%08lX.", result);
        return false;
    }

    D3D11_TEXTURE2D_DESC output = motion;
    output.Width = output_width;
    output.Height = output_height;
    output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D11Texture2D *output_texture = nullptr;
    result = device_->CreateTexture2D(&output, nullptr, &output_texture);
    if (SUCCEEDED(result))
        output_texture_ = output_texture;
    if (FAILED(result) || FAILED(device_->CreateShaderResourceView(
            output_texture_, nullptr, &output_view_)))
    {
        write_log(log, "DLSS output texture creation failed: 0x%08lX.", result);
        return false;
    }
    render_width_ = render_width;
    render_height_ = render_height;
    output_width_ = output_width;
    output_height_ = output_height;
    write_log(log, "DLSS frame resources created: %ux%u -> %ux%u.",
              render_width, render_height, output_width, output_height);
    return true;
}

ID3D11Resource *FramePipeline::prepare_color_input(
    ID3D11DeviceContext *context, ID3D11Resource *color,
    std::uint32_t render_width, std::uint32_t render_height, LogFn log)
{
    if (context == nullptr || color == nullptr || device_ == nullptr)
        return nullptr;
    ID3D11Texture2D *source = nullptr;
    if (FAILED(color->QueryInterface(IID_PPV_ARGS(&source))) || source == nullptr)
        return nullptr;
    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);
    // Use an independent input in DLAA too. Never feed a texture which is
    // still bound to the game's render pipeline directly into NGX.
    if (source_desc.Width < render_width || source_desc.Height < render_height ||
        source_desc.SampleDesc.Count != 1)
    {
        source->Release();
        return nullptr;
    }

    if (compact_color_ == nullptr || compact_color_format_ != source_desc.Format)
    {
        release(compact_color_);
        D3D11_TEXTURE2D_DESC compact = source_desc;
        compact.Width = render_width;
        compact.Height = render_height;
        compact.MipLevels = 1;
        compact.ArraySize = 1;
        compact.Usage = D3D11_USAGE_DEFAULT;
        compact.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        compact.CPUAccessFlags = 0;
        compact.MiscFlags = 0;
        ID3D11Texture2D *texture = nullptr;
        const HRESULT result = device_->CreateTexture2D(&compact, nullptr, &texture);
        if (FAILED(result) || texture == nullptr)
        {
            write_log(log, "Compact DLSS color creation failed: 0x%08lX.", result);
            source->Release();
            return nullptr;
        }
        compact_color_ = texture;
        compact_color_format_ = source_desc.Format;
        write_log(log, "Compact DLSS color created: %ux%u format %u.",
                  render_width, render_height, source_desc.Format);
    }

    const D3D11_BOX box = { 0, 0, 0, render_width, render_height, 1 };
    context->CopySubresourceRegion(compact_color_, 0, 0, 0, 0, source, 0, &box);
    source->Release();
    return compact_color_;
}

ID3D11ShaderResourceView *FramePipeline::evaluate(
    ID3D11DeviceContext *context, ID3D11Resource *color,
    const float current_view_projection[16], std::uint32_t render_width,
    std::uint32_t render_height, std::uint32_t output_width,
    std::uint32_t output_height, float jitter_x, float jitter_y,
    bool reset, NgxRuntime &ngx,
    SharedState *state, LogFn log)
{
    if (context == nullptr || color == nullptr || current_view_projection == nullptr ||
        !ensure_resources(render_width, render_height, output_width, output_height, log))
        return nullptr;
    ID3D11Resource *ngx_color = prepare_color_input(
        context, color, render_width, render_height, log);
    if (ngx_color == nullptr)
        return nullptr;

    using namespace DirectX;
    XMFLOAT4X4 current = {};
    std::memcpy(&current, current_view_projection, sizeof(current));
    const XMMATRIX current_matrix = XMLoadFloat4x4(&current);
    XMVECTOR determinant = {};
    const XMMATRIX inverse = XMMatrixInverse(&determinant, current_matrix);
    if (std::abs(XMVectorGetX(determinant)) < 1.0e-8f)
        return nullptr;
    if (has_previous_camera_)
    {
        float largest_delta = 0.0f;
        for (std::size_t i = 0; i < std::size(previous_view_projection_); ++i)
            largest_delta = (std::max)(largest_delta,
                std::abs(current_view_projection[i] - previous_view_projection_[i]));
        if (!std::isfinite(largest_delta) || largest_delta > 50.0f)
            reset = true;
    }
    if (!has_previous_camera_)
    {
        std::memcpy(previous_view_projection_, current_view_projection,
                    sizeof(previous_view_projection_));
        reset = true;
    }

    MotionConstants constants = {};
    XMStoreFloat4x4(&constants.current_inverse_view_projection, inverse);
    std::memcpy(&constants.previous_view_projection, previous_view_projection_,
                sizeof(previous_view_projection_));
    constants.render_size[0] = static_cast<float>(render_width);
    constants.render_size[1] = static_cast<float>(render_height);
    constants.current_jitter[0] = jitter_x;
    constants.current_jitter[1] = jitter_y;

    ID3D11ComputeShader *old_shader = nullptr;
    ID3D11Buffer *old_constants = nullptr;
    ID3D11ShaderResourceView *old_depth = nullptr;
    ID3D11UnorderedAccessView *old_output = nullptr;
    context->CSGetShader(&old_shader, nullptr, nullptr);
    context->CSGetConstantBuffers(0, 1, &old_constants);
    context->CSGetShaderResources(0, 1, &old_depth);
    context->CSGetUnorderedAccessViews(0, 1, &old_output);

    context->UpdateSubresource(motion_constants_, 0, nullptr, &constants, 0, 0);
    context->CSSetShader(motion_shader_, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &motion_constants_);
    context->CSSetShaderResources(0, 1, &depth_view_);
    context->CSSetUnorderedAccessViews(0, 1, &motion_uav_, nullptr);
    context->Dispatch((render_width + 7) / 8, (render_height + 7) / 8, 1);

    ID3D11UnorderedAccessView *no_uav = nullptr;
    ID3D11ShaderResourceView *no_srv = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
    context->CSSetShaderResources(0, 1, &no_srv);
    context->CSSetShader(old_shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &old_constants);
    context->CSSetShaderResources(0, 1, &old_depth);
    UINT keep_count = UINT_MAX;
    context->CSSetUnorderedAccessViews(0, 1, &old_output, &keep_count);
    release(old_shader);
    release(old_constants);
    release(old_depth);
    release(old_output);

    InterlockedExchange(&state->camera_motion_ready, 1);
    const bool succeeded = ngx.evaluate(context, ngx_color, output_texture_, scene_depth_,
                                        motion_texture_, render_width, render_height,
                                        jitter_x, jitter_y, reset, state, log);
    std::memcpy(previous_view_projection_, current_view_projection,
                sizeof(previous_view_projection_));
    has_previous_camera_ = true;
    return succeeded ? output_view_ : nullptr;
}

void FramePipeline::shutdown()
{
    release(output_view_);
    release(output_texture_);
    release(motion_uav_);
    release(motion_texture_);
    release(compact_color_);
    compact_color_format_ = 0;
    release(depth_view_);
    release(scene_depth_);
    release(motion_constants_);
    release(motion_shader_);
    release(device_);
    render_width_ = 0;
    render_height_ = 0;
    output_width_ = 0;
    output_height_ = 0;
    has_previous_camera_ = false;
}
}
