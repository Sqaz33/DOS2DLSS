#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
struct ID3D11ComputeShader;
struct ID3D11Buffer;
struct ID3D11UnorderedAccessView;

namespace dos2dlss
{
class NgxRuntime;
struct SharedState;

class FramePipeline
{
public:
    using LogFn = void (*)(const char *message);

    bool initialize(ID3D11Device *device, LogFn log);
    void set_scene_depth(ID3D11Resource *depth);
    void reset_history();
    ID3D11ShaderResourceView *evaluate(ID3D11DeviceContext *context,
                                       ID3D11Resource *color,
                                       const float current_view_projection[16],
                                       std::uint32_t render_width,
                                       std::uint32_t render_height,
                                       std::uint32_t output_width,
                                       std::uint32_t output_height,
                                       float jitter_x, float jitter_y,
                                       bool reset, NgxRuntime &ngx,
                                       SharedState *state, LogFn log);
    void shutdown();

private:
    bool ensure_resources(std::uint32_t render_width, std::uint32_t render_height,
                          std::uint32_t output_width, std::uint32_t output_height,
                          LogFn log);
    ID3D11Resource *prepare_color_input(ID3D11DeviceContext *context,
                                        ID3D11Resource *color,
                                        std::uint32_t render_width,
                                        std::uint32_t render_height,
                                        LogFn log);
    bool create_depth_view(LogFn log);

    ID3D11Device *device_ = nullptr;
    ID3D11ComputeShader *motion_shader_ = nullptr;
    ID3D11Buffer *motion_constants_ = nullptr;
    ID3D11Resource *scene_depth_ = nullptr;
    ID3D11ShaderResourceView *depth_view_ = nullptr;
    ID3D11Resource *motion_texture_ = nullptr;
    ID3D11UnorderedAccessView *motion_uav_ = nullptr;
    ID3D11Resource *compact_color_ = nullptr;
    std::uint32_t compact_color_format_ = 0;
    ID3D11Resource *output_texture_ = nullptr;
    ID3D11ShaderResourceView *output_view_ = nullptr;
    std::uint32_t render_width_ = 0;
    std::uint32_t render_height_ = 0;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    float previous_view_projection_[16] = {};
    bool has_previous_camera_ = false;
};
}
