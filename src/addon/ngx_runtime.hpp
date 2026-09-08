#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace dos2dlss
{
struct SharedState;

class NgxRuntime
{
public:
    using LogFn = void (*)(const char *message);

    bool initialize(ID3D11Device *device, const wchar_t *application_data_path,
                    SharedState *state, LogFn log);
    void sync_feature(ID3D11DeviceContext *context, int mode,
                      std::uint32_t output_width, std::uint32_t output_height,
                      SharedState *state, LogFn log);
    bool evaluate(ID3D11DeviceContext *context, ID3D11Resource *color,
                  ID3D11Resource *output, ID3D11Resource *depth,
                  ID3D11Resource *motion_vectors, std::uint32_t render_width,
                  std::uint32_t render_height, bool reset,
                  SharedState *state, LogFn log);
    void shutdown(SharedState *state, LogFn log);

private:
    void release_feature(SharedState *state, LogFn log);

    ID3D11Device *device_ = nullptr;
    NVSDK_NGX_Parameter *capabilities_ = nullptr;
    NVSDK_NGX_Parameter *parameters_ = nullptr;
    NVSDK_NGX_Handle *feature_ = nullptr;
    int feature_mode_ = 0;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    bool initialized_ = false;
};
}
