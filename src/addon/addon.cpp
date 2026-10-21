#include "dos2dlss/shared_state.hpp"
#include "frame_pipeline.hpp"
#include "ngx_runtime.hpp"
#include "context_state.hpp"

#include <windows.h>
#include <psapi.h>
#include <d3d11.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "reshade_api.hpp"
#include "reshade_events.hpp"

extern "C" __declspec(dllexport) const char *NAME = "DOS2 DLSS 0.6.2-dev";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Controls and diagnostics for the native Divinity: Original Sin 2 DLSS integration.";

namespace
{
HMODULE g_self = nullptr;
HMODULE g_reshade = nullptr;
using RegisterAddonFn = bool (*)(HMODULE, std::uint32_t);
using UnregisterAddonFn = void (*)(HMODULE);
using RegisterEventFn = void (*)(reshade::addon_event, void *);
using RegisterOverlayFn = void (*)(const char *, void (*)(reshade::api::effect_runtime *));
using GetImGuiTableFn = const void *(*)(std::uint32_t);
UnregisterAddonFn g_unregister = nullptr;

dos2dlss::SharedMapping g_mapping;
wchar_t g_config_path[MAX_PATH] = {};
wchar_t g_log_path[MAX_PATH] = {};
wchar_t g_trace_path[MAX_PATH] = {};
wchar_t g_shader_dir[MAX_PATH] = {};
wchar_t g_ngx_path[MAX_PATH] = {};
dos2dlss::NgxRuntime g_ngx;
dos2dlss::FramePipeline g_frame_pipeline;
PrivateContextState g_private_context;
thread_local bool g_internal_render = false;
struct InternalRenderScope {
    bool previous = g_internal_render;
    InternalRenderScope() { g_internal_render = true; }
    ~InternalRenderScope() { g_internal_render = previous; }
};
ID3D11Buffer *g_scaled_viewport_constants = nullptr;
ID3D11Buffer *g_scaled_pixel_viewport_constants = nullptr;
ID3D11Buffer *g_scaled_clear_constants = nullptr;
ID3D11Buffer *g_jittered_per_view_constants = nullptr;
std::size_t g_jittered_per_view_size = 0;
std::uint64_t g_jittered_per_view_source = 0;
std::uint64_t g_jittered_per_view_hash = 0;
std::uint64_t g_jittered_per_view_revision = 0;
float g_jittered_render_width = 0, g_jittered_render_height = 0;
float g_jittered_x = 0, g_jittered_y = 0;
LONG64 g_jittered_per_view_frame = -1;
volatile LONG64 g_frame_draws = 0;
volatile LONG64 g_frame_indexed_draws = 0;
volatile LONG64 g_frame_target_binds = 0;
volatile LONG g_frame_scene_w = 0;
volatile LONG g_frame_scene_h = 0;
volatile LONG g_frame_color_format = 0;
volatile LONG g_frame_depth_format = 0;
std::unordered_set<std::uint64_t> g_native_ui_resources;
volatile LONG64 g_last_dlss_evaluation_frame = -1;
bool g_diagnostics = false;
bool g_perf_legacy = false;
bool g_perf_profile = false;
std::uint64_t g_snapshot_revision = 0;
std::uint64_t g_camera_calls = 0, g_camera_cache_hits = 0;
std::uint64_t g_camera_cpu_ticks = 0, g_scoped_draws = 0, g_bypassed_draws = 0;
struct CameraCpuScope {
    LARGE_INTEGER start = {};
    CameraCpuScope() { if (g_perf_profile) QueryPerformanceCounter(&start); }
    ~CameraCpuScope() { if (start.QuadPart) { LARGE_INTEGER end; QueryPerformanceCounter(&end); g_camera_cpu_ticks += end.QuadPart-start.QuadPart; } }
};
constexpr std::uint64_t kCombineUiPixelShader = 0x7D52DF001176EB8Dull;
// This allocation path belongs to DOS2's full-resolution Scaleform/UI target
// on the supported executable. It is available before the first UI draw, while
// learning the texture from the final CombineUI pass is one frame too late.
constexpr std::uint64_t kNativeUiAllocationSite = 0x1DC57A3ull;
constexpr std::uint64_t kScaleformAllocationSite = 0x1EE8A2Eull;
bool is_ui_allocation(const std::array<std::uint64_t, 4> &sites)
{
    return std::find(sites.begin(), sites.end(), kNativeUiAllocationSite) != sites.end() ||
           std::find(sites.begin(), sites.end(), kScaleformAllocationSite) != sites.end();
}
bool is_scaleform_vertex_shader(std::uint64_t shader)
{
    return shader == 0xBD26BCCCC2592798ull || shader == 0x29309D28C1B99C79ull ||
           shader == 0xBB1B5991680FE117ull;
}
thread_local std::uint64_t g_current_color_target = 0;
thread_local int g_current_screen_level = -1;
thread_local bool g_current_native_ui_target = false;
thread_local bool g_current_gbuffer_target = false;
thread_local bool g_current_scene_depth_target = false;
thread_local bool g_current_full_scene_depth_target = false;
thread_local bool g_scene_render_phase = false;
thread_local bool g_camera_override_bound = false;
volatile LONG64 g_scene_depth_resource = 0;
std::uintptr_t g_exe_base = 0;
std::uintptr_t g_exe_end = 0;

struct PendingResourceCreation
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    std::array<std::uint64_t, 4> game_callsites = {};
    bool valid = false;
};

thread_local PendingResourceCreation g_pending_resource = {};
std::unordered_map<std::uint64_t, std::array<std::uint64_t, 4>> g_resource_callsites;

struct CapturedTarget
{
    std::uint64_t resource = 0;
    std::array<std::uint64_t, 4> creation_sites = {};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    char name[64] = {};
};

struct CapturedPass
{
    std::uint32_t target_count = 0;
    CapturedTarget targets[8] = {};
    CapturedTarget depth = {};
    std::uint32_t draws = 0;
    std::uint32_t indexed_draws = 0;
    std::uint64_t first_vertex_shader = 0;
    std::uint64_t last_vertex_shader = 0;
    std::uint64_t first_pixel_shader = 0;
    std::uint64_t last_pixel_shader = 0;
    CapturedTarget pixel_inputs[16] = {};
    std::uint64_t pixel_constant_buffers[16] = {};
    std::array<std::uint8_t, 128> vertex_constants = {};
    bool vertex_constants_valid = false;
    std::array<std::uint8_t, 240> per_view = {};
    bool per_view_valid = false;
};

struct PipelineShaders
{
    std::uint64_t vertex = 0;
    std::uint64_t pixel = 0;
    std::uint64_t compute = 0;
};

struct ShaderBlob
{
    std::vector<std::uint8_t> bytes;
    bool pixel = false;
};

struct BufferSnapshot
{
    std::array<std::uint8_t, 512> bytes = {};
    std::size_t size = 0;
    bool ui_view = false;
    std::uint64_t revision = 0;
};

struct MappedBuffer
{
    std::uint64_t resource = 0;
    const std::uint8_t *data = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

CapturedPass g_captured_passes[256] = {};
std::uint32_t g_captured_pass_count = 0;
std::int32_t g_current_captured_pass = -1;
bool g_capture_active = false;
SRWLOCK g_pipeline_lock = SRWLOCK_INIT;
std::unordered_map<std::uint64_t, PipelineShaders> g_pipeline_shaders;
std::unordered_map<std::uint64_t, ShaderBlob> g_shader_blobs;
std::unordered_map<std::uint64_t, BufferSnapshot> g_buffer_snapshots;
thread_local std::uint64_t g_bound_vertex_shader = 0;
thread_local std::uint64_t g_bound_compute_shader = 0;
thread_local std::uint64_t g_bound_pixel_shader = 0;
thread_local reshade::api::resource_view g_bound_pixel_inputs[16] = {};
thread_local reshade::api::buffer_range g_bound_vertex_constant_buffers[16] = {};
thread_local reshade::api::buffer_range g_bound_pixel_constant_buffers[16] = {};
thread_local MappedBuffer g_mapped_buffer = {};
thread_local bool g_internal_srv_bind = false;

std::uint64_t hash_shader(const void *data, std::size_t size)
{
    // This is a stable trace identifier; shader replacement never relies on
    // hash uniqueness alone.
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(size);
    hash *= 1099511628211ull;
    return hash;
}

void read_debug_name(std::uint64_t handle, char (&name)[64])
{
    if (handle == 0)
        return;
    auto *object = reinterpret_cast<ID3D11DeviceChild *>(handle);
    UINT size = static_cast<UINT>(sizeof(name) - 1);
    if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
        name[(size < sizeof(name)) ? size : sizeof(name) - 1] = '\0';
}

CapturedTarget describe_target(reshade::api::device *device, reshade::api::resource resource)
{
    CapturedTarget target = {};
    if (device == nullptr || resource.handle == 0)
        return target;
    const auto desc = device->get_resource_desc(resource);
    target.resource = resource.handle;
    target.width = desc.texture.width;
    target.height = desc.texture.height;
    target.format = static_cast<std::uint32_t>(desc.texture.format);
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto site = g_resource_callsites.find(resource.handle);
    if (site != g_resource_callsites.end())
        target.creation_sites = site->second;
    ReleaseSRWLockShared(&g_pipeline_lock);
    read_debug_name(resource.handle, target.name);
    return target;
}

std::array<std::uint64_t, 4> find_game_resource_callsites()
{
    std::array<std::uint64_t, 4> result = {};
    void *frames[32] = {};
    const USHORT count = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)),
                                                frames, nullptr);
    std::size_t output = 0;
    for (USHORT i = 0; i < count && output < result.size(); ++i)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
        if (address >= g_exe_base && address < g_exe_end)
            result[output++] = static_cast<std::uint64_t>(address - g_exe_base);
    }
    return result;
}

bool on_create_resource(reshade::api::device *, reshade::api::resource_desc &desc,
                        reshade::api::subresource_data *, reshade::api::resource_usage)
{
    g_pending_resource = {};
    if (desc.type != reshade::api::resource_type::texture_2d)
        return false;
    g_pending_resource.width = desc.texture.width;
    g_pending_resource.height = desc.texture.height;
    g_pending_resource.format = static_cast<std::uint32_t>(desc.texture.format);
    g_pending_resource.game_callsites = find_game_resource_callsites();
    g_pending_resource.valid = true;
    return false;
}

void on_init_resource(reshade::api::device *, const reshade::api::resource_desc &desc,
                      const reshade::api::subresource_data *, reshade::api::resource_usage,
                      reshade::api::resource resource)
{
    if (!g_pending_resource.valid || resource.handle == 0 ||
        desc.type != reshade::api::resource_type::texture_2d)
        return;
    if (desc.texture.width == g_pending_resource.width &&
        desc.texture.height == g_pending_resource.height &&
        static_cast<std::uint32_t>(desc.texture.format) == g_pending_resource.format)
    {
        AcquireSRWLockExclusive(&g_pipeline_lock);
        g_resource_callsites[resource.handle] = g_pending_resource.game_callsites;
        ReleaseSRWLockExclusive(&g_pipeline_lock);
    }
    g_pending_resource = {};
}

void on_destroy_resource(reshade::api::device *, reshade::api::resource resource)
{
    AcquireSRWLockExclusive(&g_pipeline_lock);
    g_native_ui_resources.erase(resource.handle);
    g_resource_callsites.erase(resource.handle);
    g_buffer_snapshots.erase(resource.handle);
    ReleaseSRWLockExclusive(&g_pipeline_lock);
}

void record_bound_state(reshade::api::command_list *cmd)
{
    if (!g_capture_active || g_current_captured_pass < 0)
        return;
    auto &pass = g_captured_passes[g_current_captured_pass];
    if (pass.first_vertex_shader == 0)
        pass.first_vertex_shader = g_bound_vertex_shader;
    if (pass.first_pixel_shader == 0)
        pass.first_pixel_shader = g_bound_pixel_shader;
    pass.last_vertex_shader = g_bound_vertex_shader;
    pass.last_pixel_shader = g_bound_pixel_shader;
    if (cmd == nullptr || (pass.draws + pass.indexed_draws) != 1)
        return;
    auto *device = cmd->get_device();
    for (std::uint32_t i = 0; i < std::size(g_bound_pixel_inputs); ++i)
    {
        if (g_bound_pixel_inputs[i].handle != 0)
            pass.pixel_inputs[i] = describe_target(
                device, device->get_resource_from_view(g_bound_pixel_inputs[i]));
        pass.pixel_constant_buffers[i] = g_bound_pixel_constant_buffers[i].buffer.handle;
    }
    const auto &vertex_constants = g_bound_vertex_constant_buffers[0];
    if (vertex_constants.buffer.handle != 0 && vertex_constants.offset == 0)
    {
        AcquireSRWLockShared(&g_pipeline_lock);
        const auto entry = g_buffer_snapshots.find(vertex_constants.buffer.handle);
        if (entry != g_buffer_snapshots.end() &&
            entry->second.size >= pass.vertex_constants.size())
        {
            std::memcpy(pass.vertex_constants.data(), entry->second.bytes.data(),
                        pass.vertex_constants.size());
            pass.vertex_constants_valid = true;
        }
        ReleaseSRWLockShared(&g_pipeline_lock);
    }
    const auto &per_view = g_bound_pixel_constant_buffers[12];
    if (per_view.buffer.handle != 0 && per_view.offset == 0)
    {
        AcquireSRWLockShared(&g_pipeline_lock);
        const auto entry = g_buffer_snapshots.find(per_view.buffer.handle);
        if (entry != g_buffer_snapshots.end() && entry->second.size >= pass.per_view.size())
        {
            std::memcpy(pass.per_view.data(), entry->second.bytes.data(), pass.per_view.size());
            pass.per_view_valid = true;
        }
        ReleaseSRWLockShared(&g_pipeline_lock);
    }
}

void save_buffer_snapshot(std::uint64_t resource, const void *data,
                          std::uint64_t offset, std::uint64_t size)
{
    if (resource == 0 || data == nullptr || offset != 0 || size == 0)
        return;
    BufferSnapshot snapshot;
    snapshot.size = static_cast<std::size_t>((std::min)(size,
        static_cast<std::uint64_t>(snapshot.bytes.size())));
    std::memcpy(snapshot.bytes.data(), data, snapshot.size);
    // Native font/layout passes use an identity View and zero near/far planes.
    // They can target pooled textures allocated by the ordinary scene allocator.
    if (snapshot.size >= 224) {
        float view[56] = {};
        std::memcpy(view, snapshot.bytes.data(), sizeof(view));
        snapshot.ui_view = view[52] == 0.0f && view[53] == 0.0f;
        for (int i = 0; i < 16 && snapshot.ui_view; ++i)
            snapshot.ui_view = view[i] == ((i % 5 == 0) ? 1.0f : 0.0f);
    }
    AcquireSRWLockExclusive(&g_pipeline_lock);
    snapshot.revision = ++g_snapshot_revision;
    g_buffer_snapshots[resource] = snapshot;
    ReleaseSRWLockExclusive(&g_pipeline_lock);
}

void on_map_buffer_region(reshade::api::device *device, reshade::api::resource resource,
                          std::uint64_t offset, std::uint64_t size,
                          reshade::api::map_access, void **data)
{
    g_mapped_buffer = {};
    if (device == nullptr || resource.handle == 0 || data == nullptr || *data == nullptr)
        return;
    if (size == UINT64_MAX)
    {
        const auto desc = device->get_resource_desc(resource);
        if (desc.type != reshade::api::resource_type::buffer || desc.buffer.size <= offset)
            return;
        size = desc.buffer.size - offset;
    }
    g_mapped_buffer = { resource.handle, static_cast<const std::uint8_t *>(*data), offset, size };
}

void on_unmap_buffer_region(reshade::api::device *, reshade::api::resource resource)
{
    if (g_mapped_buffer.resource == resource.handle)
        save_buffer_snapshot(resource.handle, g_mapped_buffer.data,
                             g_mapped_buffer.offset, g_mapped_buffer.size);
    g_mapped_buffer = {};
}

bool on_update_buffer_region(reshade::api::device *, const void *data,
                             reshade::api::resource resource, std::uint64_t offset,
                             std::uint64_t size)
{
    save_buffer_snapshot(resource.handle, data, offset, size);
    return false;
}

void log_line(const char *format, ...)
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, g_log_path, L"a") != 0 || file == nullptr)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

void log_message(const char *message)
{
    log_line("%s", message != nullptr ? message : "");
}

void make_sibling_path(HMODULE module, const wchar_t *leaf, wchar_t (&out)[MAX_PATH])
{
    GetModuleFileNameW(module, out, MAX_PATH);
    if (wchar_t *slash = wcsrchr(out, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - out), leaf);
}

int load_mode()
{
    const int value = GetPrivateProfileIntW(L"DOS2DLSS", L"Mode", 2, g_config_path);
    return value >= 0 && value <= 4 ? value : 2;
}

void save_int(const wchar_t *key, int value)
{
    wchar_t text[24] = {};
    swprintf_s(text, L"%d", value);
    WritePrivateProfileStringW(L"DOS2DLSS", key, text, g_config_path);
}

bool register_with_reshade()
{
    HMODULE modules[1024] = {};
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return false;

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
    {
        auto reg = reinterpret_cast<RegisterAddonFn>(GetProcAddress(modules[i], "ReShadeRegisterAddon"));
        if (reg == nullptr)
            continue;

        // ReShade 6.8 uses add-on API 18. Refusing older layouts is safer than
        // calling a shifted C++ vtable in the game process.
        if (!reg(g_self, 18))
            continue;

        g_reshade = modules[i];
        g_unregister = reinterpret_cast<UnregisterAddonFn>(
            GetProcAddress(g_reshade, "ReShadeUnregisterAddon"));
        return true;
    }
    return false;
}

void update_status(const wchar_t *text)
{
    if (auto *state = g_mapping.get())
        wcscpy_s(state->addon_status, text);
}

void on_init_device(reshade::api::device *device)
{
    if (device == nullptr)
        return;
    if (device->get_api() == reshade::api::device_api::d3d11)
    {
        auto *native_device = reinterpret_cast<ID3D11Device *>(device->get_native());
        if (!g_private_context.initialize(native_device)) {
            update_status(L"D3D11.1 context isolation unavailable; DLSS disabled.");
            return;
        }
        g_frame_pipeline.initialize(native_device, &log_message);
        if (g_ngx.initialize(native_device, g_ngx_path, g_mapping.get(), &log_message))
            update_status(L"D3D11 active; NVIDIA NGX initialized.");
        else
            update_status(L"D3D11 active; NVIDIA NGX initialization failed. See dos2-dlss.log.");
    }
    else
    {
        auto *state = g_mapping.get();
        if (state == nullptr || InterlockedCompareExchange(&state->ngx_initialized, 0, 0) == 0)
            update_status(L"Unsupported graphics API: DOS2DLSS requires D3D11.");
    }
}

void on_destroy_device(reshade::api::device *)
{
    if (g_scaled_pixel_viewport_constants != nullptr) {
        g_scaled_pixel_viewport_constants->Release();
        g_scaled_pixel_viewport_constants = nullptr;
    }
    if (g_scaled_clear_constants != nullptr) {
        g_scaled_clear_constants->Release();
        g_scaled_clear_constants = nullptr;
    }
    if (g_scaled_viewport_constants != nullptr)
    {
        g_scaled_viewport_constants->Release();
        g_scaled_viewport_constants = nullptr;
    }
    if (g_jittered_per_view_constants != nullptr)
    {
        g_jittered_per_view_constants->Release();
        g_jittered_per_view_constants = nullptr;
    }
    g_jittered_per_view_size = 0;
    g_jittered_per_view_source = 0;
    g_jittered_per_view_hash = 0;
    g_jittered_per_view_frame = -1;
    InterlockedExchange64(&g_last_dlss_evaluation_frame, -1);
    g_camera_override_bound = false;
    AcquireSRWLockExclusive(&g_pipeline_lock);
    g_native_ui_resources.clear();
    ReleaseSRWLockExclusive(&g_pipeline_lock);
    g_frame_pipeline.shutdown();
    g_ngx.shutdown(g_mapping.get(), &log_message);
    g_private_context.shutdown();
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool)
{
    if (swapchain == nullptr || swapchain->get_back_buffer_count() == 0)
        return;
    auto *device = swapchain->get_device();
    if (device == nullptr)
        return;
    const auto desc = device->get_resource_desc(swapchain->get_back_buffer(0));
    if (auto *state = g_mapping.get())
    {
        InterlockedExchange(&state->backbuffer_width, static_cast<LONG>(desc.texture.width));
        InterlockedExchange(&state->backbuffer_height, static_cast<LONG>(desc.texture.height));
    }
}

int screen_level(std::uint32_t width, std::uint32_t height,
                 std::uint32_t output_width, std::uint32_t output_height)
{
    for (int level = 0; level <= 6; ++level)
    {
        if (width == (output_width >> level) && height == (output_height >> level))
            return level;
    }
    return -1;
}

bool is_fullscreen_vertex_shader(std::uint64_t shader)
{
    switch (shader)
    {
    case 0x733270527D136859ull:
    case 0x7C28E4C7E62FE80Bull:
    case 0x824764AD6D95C632ull:
    case 0x8C01621FFC817F71ull:
    case 0xA324AB6FE694FE02ull:
    case 0xB22F95DD13AA995Aull:
    case 0xD0938263903A6857ull:
    case 0xE23AC6CD78CD13BCull:
    case 0xFC1811A3281055CAull:
    case 0xA24881A89C311963ull: // EoCOutlineBlurH
    case 0xC168C0D066FEB718ull: // EoCOutlineBlurV
        return true;
    default:
        return false;
    }
}

float halton(std::uint64_t index, std::uint32_t base)
{
    float value = 0.0f;
    float fraction = 1.0f;
    while (index != 0)
    {
        fraction /= static_cast<float>(base);
        value += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return value;
}

void get_frame_jitter(dos2dlss::SharedState *state, float &x, float &y)
{
    x = 0.0f;
    y = 0.0f;
    if (state == nullptr)
        return;
    const LONG mode = InterlockedCompareExchange(&state->quality_mode, 0, 0);
    if (mode <= static_cast<LONG>(dos2dlss::QualityMode::off))
        return;
    static constexpr std::uint32_t phases[] = { 1, 8, 18, 24, 32 };
    const auto phase_count = phases[(std::min)(4L, mode)];
    const auto frame = static_cast<std::uint64_t>(
        InterlockedCompareExchange64(&state->frame_number, 0, 0));
    const auto sample = frame % phase_count + 1;
    x = halton(sample, 2) - 0.5f;
    y = halton(sample, 3) - 0.5f;
}

void bind_viewport_uv_constants(ID3D11DeviceContext *context, bool scaled,
                                float scale_x, float scale_y, bool pixel = false)
{
    const auto &range = pixel ? g_bound_pixel_constant_buffers[0] : g_bound_vertex_constant_buffers[0];
    if (context == nullptr || range.buffer.handle == 0 || range.offset != 0)
        return;

    auto *original = reinterpret_cast<ID3D11Buffer *>(range.buffer.handle);
    if (pixel) context->PSSetConstantBuffers(0, 1, &original);
    else context->VSSetConstantBuffers(0, 1, &original);
    if (!scaled)
        return;

    BufferSnapshot snapshot;
    bool found = false;
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto entry = g_buffer_snapshots.find(range.buffer.handle);
    if (entry != g_buffer_snapshots.end() && entry->second.size >= sizeof(float) * 4)
    {
        snapshot = entry->second;
        found = true;
    }
    ReleaseSRWLockShared(&g_pipeline_lock);
    if (!found)
        return;

    auto *&scaled_buffer = pixel ? g_scaled_pixel_viewport_constants : g_scaled_viewport_constants;
    if (scaled_buffer == nullptr)
    {
        ID3D11Device *device = nullptr;
        context->GetDevice(&device);
        if (device == nullptr)
            return;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(snapshot.bytes.size());
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT result = device->CreateBuffer(&desc, nullptr, &scaled_buffer);
        device->Release();
        if (FAILED(result))
        {
            log_line("Viewport UV constant buffer creation failed: 0x%08lX.", result);
            return;
        }
    }

    auto *values = reinterpret_cast<float *>(snapshot.bytes.data());
    values[0] *= scale_x;
    values[1] *= scale_y;
    values[2] *= scale_x;
    values[3] *= scale_y;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(scaled_buffer, 0, D3D11_MAP_WRITE_DISCARD,
                            0, &mapped)))
        return;
    std::memcpy(mapped.pData, snapshot.bytes.data(), snapshot.bytes.size());
    context->Unmap(scaled_buffer, 0);
    if (pixel) context->PSSetConstantBuffers(0, 1, &scaled_buffer);
    else context->VSSetConstantBuffers(0, 1, &scaled_buffer);
}

void restore_camera_constants(ID3D11DeviceContext *context)
{
    if (context == nullptr || !g_camera_override_bound)
        return;
    const auto &vertex = g_bound_vertex_constant_buffers[12];
    const auto &pixel = g_bound_pixel_constant_buffers[12];
    if (vertex.buffer.handle != 0 && vertex.offset == 0)
    {
        auto *buffer = reinterpret_cast<ID3D11Buffer *>(vertex.buffer.handle);
        context->VSSetConstantBuffers(12, 1, &buffer);
    }
    if (pixel.buffer.handle != 0 && pixel.offset == 0)
    {
        auto *buffer = reinterpret_cast<ID3D11Buffer *>(pixel.buffer.handle);
        context->PSSetConstantBuffers(12, 1, &buffer);
    }
    g_camera_override_bound = false;
}

bool bind_jittered_camera_constants(ID3D11DeviceContext *context,
                                    dos2dlss::SharedState *state,
                                    float jitter_x, float jitter_y,
                                    float render_width, float render_height)
{
    CameraCpuScope cpu_profile;
    if (g_perf_profile) ++g_camera_calls;
    if (context == nullptr || state == nullptr || render_width <= 0.0f ||
        render_height <= 0.0f)
        return false;
    const auto &vertex = g_bound_vertex_constant_buffers[12];
    const auto &pixel = g_bound_pixel_constant_buffers[12];
    const auto source = vertex.buffer.handle != 0 && vertex.offset == 0 ? vertex : pixel;
    if (source.buffer.handle == 0 || source.offset != 0)
        return false;

    const auto frame = InterlockedCompareExchange64(&state->frame_number, 0, 0);
    BufferSnapshot snapshot;
    bool found = false;
    bool cache_hit = false;
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto entry = g_buffer_snapshots.find(source.buffer.handle);
    if (entry != g_buffer_snapshots.end() && entry->second.size >= sizeof(float) * 48)
    {
        cache_hit = !g_perf_legacy && g_jittered_per_view_constants != nullptr &&
            g_jittered_per_view_frame == frame && g_jittered_per_view_source == source.buffer.handle &&
            g_jittered_per_view_revision == entry->second.revision &&
            g_jittered_render_width == render_width && g_jittered_render_height == render_height &&
            g_jittered_x == jitter_x && g_jittered_y == jitter_y;
        if (!cache_hit) snapshot = entry->second;
        found = true;
    }
    ReleaseSRWLockShared(&g_pipeline_lock);
    if (cache_hit) {
        if (g_perf_profile) ++g_camera_cache_hits;
        context->VSSetConstantBuffers(12, 1, &g_jittered_per_view_constants);
        context->PSSetConstantBuffers(12, 1, &g_jittered_per_view_constants);
        g_camera_override_bound = true;
        InterlockedExchange(&state->camera_jitter_ready, 1);
        return true;
    }
    if (!found || snapshot.size % 16 != 0)
        return false;

    auto *values = reinterpret_cast<float *>(snapshot.bytes.data());
    for (std::size_t i = 0; i < 48; ++i)
        if (!std::isfinite(values[i]))
            return false;

    // DOS2 stores column-vector matrices in row-major memory. Add the clip-space
    // offset to the projection and view-projection rows, while retaining the
    // original unjittered buffer for motion-vector reconstruction.
    const float clip_x = 2.0f * jitter_x / render_width;
    const float clip_y = -2.0f * jitter_y / render_height;
    const auto jitter_matrix = [clip_x, clip_y](float *matrix)
    {
        for (std::size_t column = 0; column < 4; ++column)
        {
            matrix[column] += clip_x * matrix[12 + column];
            matrix[4 + column] += clip_y * matrix[12 + column];
        }
    };
    jitter_matrix(values + 16);
    jitter_matrix(values + 32);

    const auto contents_hash = g_perf_legacy ? hash_shader(snapshot.bytes.data(), snapshot.size) : snapshot.revision;
    const bool upload = g_jittered_per_view_constants == nullptr ||
        g_jittered_per_view_size != snapshot.size ||
        g_jittered_per_view_source != source.buffer.handle ||
        g_jittered_per_view_hash != contents_hash ||
        g_jittered_render_width != render_width || g_jittered_render_height != render_height ||
        g_jittered_x != jitter_x || g_jittered_y != jitter_y ||
        g_jittered_per_view_frame != frame;
    if (g_jittered_per_view_constants == nullptr ||
        g_jittered_per_view_size != snapshot.size)
    {
        if (g_jittered_per_view_constants != nullptr)
            g_jittered_per_view_constants->Release();
        g_jittered_per_view_constants = nullptr;
        ID3D11Device *device = nullptr;
        context->GetDevice(&device);
        if (device == nullptr)
            return false;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(snapshot.size);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT created = device->CreateBuffer(
            &desc, nullptr, &g_jittered_per_view_constants);
        device->Release();
        if (FAILED(created))
        {
            log_line("Jittered camera constant buffer creation failed: 0x%08lX.", created);
            return false;
        }
        g_jittered_per_view_size = snapshot.size;
    }
    if (upload)
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(g_jittered_per_view_constants, 0,
                                D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, snapshot.bytes.data(), snapshot.size);
        context->Unmap(g_jittered_per_view_constants, 0);
        g_jittered_per_view_source = source.buffer.handle;
        g_jittered_per_view_hash = contents_hash;
        g_jittered_per_view_revision = snapshot.revision;
        g_jittered_render_width = render_width;
        g_jittered_render_height = render_height;
        g_jittered_x = jitter_x; g_jittered_y = jitter_y;
        g_jittered_per_view_frame = frame;
    }
    context->VSSetConstantBuffers(12, 1, &g_jittered_per_view_constants);
    context->PSSetConstantBuffers(12, 1, &g_jittered_per_view_constants);
    g_camera_override_bound = true;
    InterlockedExchange(&state->camera_jitter_ready, 1);
    return true;
}

bool has_scaled_scene_input(ID3D11DeviceContext *context, dos2dlss::SharedState *state)
{
    ID3D11ShaderResourceView *view = nullptr;
    context->PSGetShaderResources(0, 1, &view);
    if (view == nullptr) return false;
    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    view->Release();
    ID3D11Texture2D *texture = nullptr;
    resource->QueryInterface(IID_PPV_ARGS(&texture));
    bool ui = false;
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto key = reinterpret_cast<std::uint64_t>(resource);
    const auto origin = g_resource_callsites.find(key);
    ui = g_native_ui_resources.contains(key) ||
        (origin != g_resource_callsites.end() && is_ui_allocation(origin->second));
    ReleaseSRWLockShared(&g_pipeline_lock);
    resource->Release();
    if (texture == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->Release();
    return !ui && screen_level(desc.Width, desc.Height,
        state->backbuffer_width, state->backbuffer_height) >= 0;
}

void prepare_draw_resolution(reshade::api::command_list *cmd)
{
    if (!g_scene_render_phase) return;
    auto *state = g_mapping.get();
    if (cmd == nullptr || state == nullptr)
        return;
    const LONG mode = InterlockedCompareExchange(&state->quality_mode, 0, 0);
    if (mode <= static_cast<LONG>(dos2dlss::QualityMode::off))
        return;

    const LONG render_width = InterlockedCompareExchange(&state->render_width, 0, 0);
    const LONG render_height = InterlockedCompareExchange(&state->render_height, 0, 0);
    const LONG output_width = InterlockedCompareExchange(&state->backbuffer_width, 0, 0);
    const LONG output_height = InterlockedCompareExchange(&state->backbuffer_height, 0, 0);
    if (render_width <= 0 || render_height <= 0 || output_width <= 0 || output_height <= 0)
        return;

    auto *context = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    if (context == nullptr)
        return;

    // Scaleform uses per-widget viewports and scissor rectangles, including
    // the minimap. The target texture size is not the widget viewport size.
    if (g_current_native_ui_target)
    {
        restore_camera_constants(context);
        return;
    }

    // DLAA keeps the native render dimensions, but still needs the same
    // sub-pixel sample position that is supplied to NGX. Restrict the shifted
    // viewport to the G-buffer pass so UI, shadow maps and screen-space passes
    // retain their exact pixel coordinates.
    if (mode == static_cast<LONG>(dos2dlss::QualityMode::dlaa))
    {
        if (!g_current_scene_depth_target || is_fullscreen_vertex_shader(g_bound_vertex_shader))
        {
            restore_camera_constants(context);
            return;
        }
        float jitter_x = 0.0f;
        float jitter_y = 0.0f;
        get_frame_jitter(state, jitter_x, jitter_y);
        bind_jittered_camera_constants(context, state, jitter_x, jitter_y,
                                       static_cast<float>(output_width),
                                       static_cast<float>(output_height));
        return;
    }

    if (render_width >= output_width || render_height >= output_height)
    {
        restore_camera_constants(context);
        return;
    }
    const bool fullscreen = is_fullscreen_vertex_shader(g_bound_vertex_shader);
    if (!g_current_scene_depth_target || fullscreen)
        restore_camera_constants(context);
    // The final combine samples our already full-resolution DLSS output and
    // the native UI. Scaling its shared interpolator would zoom both inputs.
    const bool scale_uv = fullscreen && !g_current_native_ui_target &&
                          g_bound_pixel_shader != kCombineUiPixelShader &&
                          (g_current_screen_level >= 0 || has_scaled_scene_input(context, state));
    bind_viewport_uv_constants(context, scale_uv,
        static_cast<float>(render_width) / static_cast<float>(output_width),
        static_cast<float>(render_height) / static_cast<float>(output_height));
    // The outline PS clamps against VPtoRTScaleBias too; VS-only correction
    // samples outside the active rectangle and reintroduces stale mask pixels.
    if (scale_uv && g_bound_pixel_shader == 0x55D5D37A212C2405ull)
        bind_viewport_uv_constants(context, true,
            static_cast<float>(render_width) / static_cast<float>(output_width),
            static_cast<float>(render_height) / static_cast<float>(output_height), true);
    if ((!g_perf_legacy && g_bound_pixel_shader == kCombineUiPixelShader) ||
        (g_current_screen_level < 0 && !g_current_native_ui_target))
        return;
    const bool scale_scene = !g_current_native_ui_target &&
                             g_bound_pixel_shader != kCombineUiPixelShader;
    const LONG native_width = g_current_screen_level >= 0
        ? output_width >> g_current_screen_level : output_width;
    const LONG native_height = g_current_screen_level >= 0
        ? output_height >> g_current_screen_level : output_height;
    const LONG desired_width = scale_scene
        ? (std::max)(1L, render_width >> g_current_screen_level) : native_width;
    const LONG desired_height = scale_scene
        ? (std::max)(1L, render_height >> g_current_screen_level) : native_height;
    const float width = static_cast<float>(desired_width);
    const float height = static_cast<float>(desired_height);
    if (g_current_scene_depth_target && !g_current_native_ui_target && !fullscreen)
    {
        float jitter_x = 0.0f;
        float jitter_y = 0.0f;
        get_frame_jitter(state, jitter_x, jitter_y);
        bind_jittered_camera_constants(context, state, jitter_x, jitter_y,
                                       static_cast<float>(render_width),
                                       static_cast<float>(render_height));
    }
    // Outline masks use object-sized viewports/scissors. Scale their original
    // rectangle, retaining its origin, rather than replacing it with the screen.
    const float sx = width / static_cast<float>(native_width);
    const float sy = height / static_cast<float>(native_height);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT nv = static_cast<UINT>(std::size(viewports)), ns = static_cast<UINT>(std::size(scissors));
    context->RSGetViewports(&nv, viewports);
    context->RSGetScissorRects(&ns, scissors);
    for (UINT i = 0; i < nv; ++i) {
        viewports[i].TopLeftX *= sx; viewports[i].TopLeftY *= sy;
        viewports[i].Width *= sx; viewports[i].Height *= sy;
    }
    for (UINT i = 0; i < ns; ++i) {
        scissors[i].left = static_cast<LONG>(std::floor(scissors[i].left * sx));
        scissors[i].top = static_cast<LONG>(std::floor(scissors[i].top * sy));
        scissors[i].right = static_cast<LONG>(std::ceil(scissors[i].right * sx));
        scissors[i].bottom = static_cast<LONG>(std::ceil(scissors[i].bottom * sy));
    }
    context->RSSetViewports(nv, viewports);
    context->RSSetScissorRects(ns, scissors);
}

void evaluate_native_dlss(reshade::api::command_list *cmd)
{
    auto *state = g_mapping.get();
    if (cmd == nullptr || state == nullptr || g_bound_pixel_shader != kCombineUiPixelShader ||
        InterlockedCompareExchange(&state->quality_mode, 0, 0) <=
            static_cast<LONG>(dos2dlss::QualityMode::off) ||
        InterlockedCompareExchange(&state->ngx_feature_created, 0, 0) == 0)
        return;

    const LONG64 frame = InterlockedCompareExchange64(&state->frame_number, 0, 0);
    if (InterlockedCompareExchange64(&g_last_dlss_evaluation_frame, 0, 0) == frame)
        return;

    float current_view_projection[16] = {};
    const auto per_view = g_bound_pixel_constant_buffers[12];
    if (per_view.buffer.handle == 0 || per_view.offset != 0)
        return;
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto snapshot = g_buffer_snapshots.find(per_view.buffer.handle);
    const bool have_camera = snapshot != g_buffer_snapshots.end() && snapshot->second.size >= 192;
    if (have_camera)
        std::memcpy(current_view_projection, snapshot->second.bytes.data() + 128,
                    sizeof(current_view_projection));
    ReleaseSRWLockShared(&g_pipeline_lock);
    if (!have_camera)
        return;

    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    get_frame_jitter(state, jitter_x, jitter_y);
    state->jitter_x = jitter_x;
    state->jitter_y = jitter_y;

    auto *context = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    if (context == nullptr)
        return;
    ID3D11ShaderResourceView *ui_view = nullptr;
    context->PSGetShaderResources(1, 1, &ui_view);
    if (ui_view != nullptr)
    {
        ID3D11Resource *ui_resource = nullptr;
        ui_view->GetResource(&ui_resource);
        ui_view->Release();
        if (ui_resource != nullptr)
        {
            AcquireSRWLockExclusive(&g_pipeline_lock);
            g_native_ui_resources.insert(reinterpret_cast<std::uint64_t>(ui_resource));
            ReleaseSRWLockExclusive(&g_pipeline_lock);
            ui_resource->Release();
        }
    }
    ID3D11ShaderResourceView *input_view = nullptr;
    // Read a live COM reference; cached raw descriptor handles can be destroyed
    // during level loads. DrawRasterScope restores this slot after composition.
    context->PSGetShaderResources(0, 1, &input_view);
    if (input_view == nullptr)
        return;
    ID3D11Resource *input_color = nullptr;
    input_view->GetResource(&input_color);
    input_view->Release();
    if (input_color == nullptr)
        return;

    const auto render_width = static_cast<std::uint32_t>(state->render_width);
    const auto render_height = static_cast<std::uint32_t>(state->render_height);
    const auto output_width = static_cast<std::uint32_t>(state->backbuffer_width);
    const auto output_height = static_cast<std::uint32_t>(state->backbuffer_height);
    const bool reset = InterlockedExchange(&state->reset_requested, 0) != 0;
    ID3D11ShaderResourceView *output_view = nullptr;
    {
        InternalRenderScope internal;
        PrivateContextState::Scope pipeline(g_private_context);
        if (pipeline.active()) output_view = g_frame_pipeline.evaluate(
            context, input_color, current_view_projection, render_width, render_height,
            output_width, output_height, jitter_x, jitter_y,
            reset, g_ngx, state, &log_message);
    }
    input_color->Release();
    if (output_view != nullptr)
    {
        g_internal_srv_bind = true;
        context->PSSetShaderResources(0, 1, &output_view);
        g_internal_srv_bind = false;
        InterlockedExchange64(&g_last_dlss_evaluation_frame, frame);
        InterlockedExchange(&state->native_dlss_active, 1);
    }
}

// All viewport overrides are local to one draw. DOS2 caches raster state;
// leaving an override bound stretches later UI even if its target is known.
thread_local bool g_submitting_draw = false;
bool is_ui_draw()
{
    if (g_current_native_ui_target) return true;
    // Learn both the main UI surface and its pooled glyph/filter intermediates
    // from their producer, before any scene-sized viewport heuristic runs.
    if (is_scaleform_vertex_shader(g_bound_vertex_shader)) {
        g_current_native_ui_target = true;
        if (g_current_color_target != 0) {
            AcquireSRWLockExclusive(&g_pipeline_lock);
            g_native_ui_resources.insert(g_current_color_target);
            ReleaseSRWLockExclusive(&g_pipeline_lock);
        }
    }
    if (g_current_native_ui_target) return true;
    const auto &range = g_bound_vertex_constant_buffers[12];
    bool result = false;
    if (range.buffer.handle != 0 && range.offset == 0) {
        AcquireSRWLockShared(&g_pipeline_lock);
        const auto found = g_buffer_snapshots.find(range.buffer.handle);
        result = found != g_buffer_snapshots.end() && found->second.ui_view;
        ReleaseSRWLockShared(&g_pipeline_lock);
    }
    return result && g_bound_pixel_shader != kCombineUiPixelShader;
}
void begin_scene_draw_phase()
{
    // The depth prepass precedes MRT G-buffer binding. It must use the same
    // rectangle, but UI/stencil geometry sharing this depth surface must not.
    if (!g_scene_render_phase && g_current_full_scene_depth_target &&
        g_bound_vertex_shader != 0 && !is_fullscreen_vertex_shader(g_bound_vertex_shader) &&
        !is_ui_draw()) {
        g_scene_render_phase = true;
        g_current_scene_depth_target = true;
    }
}
struct DrawRasterScope
{
    ID3D11DeviceContext *context;
    ID3D11ShaderResourceView *scene_input = nullptr;
    ID3D11Buffer *vertex_globals = nullptr;
    ID3D11Buffer *pixel_globals = nullptr;
    bool restore_globals = false;
    bool restore_scene_input = false;
    bool restore_raster = true;
    UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    explicit DrawRasterScope(reshade::api::command_list *cmd)
        : context(reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native()))
    {
        if (g_perf_profile) ++g_scoped_draws;
        restore_scene_input = g_bound_pixel_shader == kCombineUiPixelShader;
        if (restore_scene_input)
            context->PSGetShaderResources(0, 1, &scene_input);
        restore_globals = is_fullscreen_vertex_shader(g_bound_vertex_shader);
        if (restore_globals) {
            context->VSGetConstantBuffers(0, 1, &vertex_globals);
            context->PSGetConstantBuffers(0, 1, &pixel_globals);
        }
        const auto *state = g_mapping.get();
        restore_raster = g_perf_legacy || (state != nullptr && state->quality_mode > 1 &&
            g_current_screen_level >= 0 && g_bound_pixel_shader != kCombineUiPixelShader);
        if (restore_raster) {
            context->RSGetViewports(&viewport_count, viewports);
            context->RSGetScissorRects(&scissor_count, scissors);
        }
        g_submitting_draw = true;
    }
    ~DrawRasterScope()
    {
        restore_camera_constants(context);
        if (restore_globals) {
            context->VSSetConstantBuffers(0, 1, &vertex_globals);
            context->PSSetConstantBuffers(0, 1, &pixel_globals);
            if (vertex_globals) vertex_globals->Release();
            if (pixel_globals) pixel_globals->Release();
        }
        if (restore_scene_input)
        {
            g_internal_srv_bind = true;
            context->PSSetShaderResources(0, 1, &scene_input);
            g_internal_srv_bind = false;
            if (scene_input != nullptr) scene_input->Release();
        }
        if (restore_raster) {
            context->RSSetViewports(viewport_count, viewports);
            context->RSSetScissorRects(scissor_count, scissors);
        }
        g_submitting_draw = false;
    }
};

bool draw_needs_override(const dos2dlss::SharedState *state)
{
    if (state == nullptr || state->quality_mode <= 0) return false;
    if (g_perf_legacy || g_bound_pixel_shader == kCombineUiPixelShader) return true;
    const bool fullscreen = is_fullscreen_vertex_shader(g_bound_vertex_shader);
    if (state->quality_mode == 1) return g_current_scene_depth_target && !fullscreen;
    // Shadow maps and offscreen object/UI previews retain their native raster
    // state. No DLSS input or jitter is modified by these geometry draws.
    return g_current_screen_level >= 0 || fullscreen;
}

bool on_draw(reshade::api::command_list *cmd, std::uint32_t vertices,
             std::uint32_t instances, std::uint32_t first_vertex, std::uint32_t first_instance)
{
    if (g_submitting_draw)
        return false;
    begin_scene_draw_phase();
    const auto *state = g_mapping.get();
    if (!g_scene_render_phase || is_ui_draw() || !draw_needs_override(state)) {
        if (g_perf_profile) ++g_bypassed_draws;
        if (g_diagnostics) InterlockedIncrement64(&g_frame_draws);
        if (g_capture_active && g_current_captured_pass >= 0) {
            ++g_captured_passes[g_current_captured_pass].draws;
            record_bound_state(cmd);
        }
        return false;
    }
    DrawRasterScope raster(cmd);
    prepare_draw_resolution(cmd);
    evaluate_native_dlss(cmd);
    if (g_diagnostics)
        InterlockedIncrement64(&g_frame_draws);
    if (g_capture_active && g_current_captured_pass >= 0)
    {
        ++g_captured_passes[g_current_captured_pass].draws;
        record_bound_state(cmd);
    }
    raster.context->DrawInstanced(vertices, instances, first_vertex, first_instance);
    if (g_bound_pixel_shader == kCombineUiPixelShader) g_scene_render_phase = false;
    return true;
}

bool on_draw_indexed(reshade::api::command_list *cmd, std::uint32_t indices,
                     std::uint32_t instances, std::uint32_t first_index,
                     std::int32_t vertex_offset, std::uint32_t first_instance)
{
    if (g_submitting_draw)
        return false;
    begin_scene_draw_phase();
    const auto *state = g_mapping.get();
    if (!g_scene_render_phase || is_ui_draw() || !draw_needs_override(state)) {
        if (g_perf_profile) ++g_bypassed_draws;
        if (g_diagnostics) InterlockedIncrement64(&g_frame_indexed_draws);
        if (g_capture_active && g_current_captured_pass >= 0) {
            ++g_captured_passes[g_current_captured_pass].indexed_draws;
            record_bound_state(cmd);
        }
        return false;
    }
    DrawRasterScope raster(cmd);
    prepare_draw_resolution(cmd);
    evaluate_native_dlss(cmd);
    if (g_diagnostics)
        InterlockedIncrement64(&g_frame_indexed_draws);
    if (g_capture_active && g_current_captured_pass >= 0)
    {
        ++g_captured_passes[g_current_captured_pass].indexed_draws;
        record_bound_state(cmd);
    }
    raster.context->DrawIndexedInstanced(indices, instances, first_index, vertex_offset, first_instance);
    if (g_bound_pixel_shader == kCombineUiPixelShader) g_scene_render_phase = false;
    return true;
}

bool on_dispatch(reshade::api::command_list *cmd, std::uint32_t,
                 std::uint32_t, std::uint32_t)
{
    static thread_local bool submitting = false;
    if (g_internal_render || !g_scene_render_phase || submitting || cmd == nullptr || g_bound_compute_shader != 0xB821CC2B8D8E719Dull)
        return false;
    auto *state = g_mapping.get();
    if (state == nullptr || state->quality_mode <= static_cast<LONG>(dos2dlss::QualityMode::dlaa) ||
        state->render_width <= 0 || state->render_height <= 0)
        return false;
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(cmd->get_native());
    ID3D11UnorderedAccessView *view = nullptr;
    context->CSGetUnorderedAccessViews(0, 1, &view);
    if (view == nullptr) return false;
    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    view->Release();
    ID3D11Texture2D *texture = nullptr;
    resource->QueryInterface(IID_PPV_ARGS(&texture));
    bool native_ui = false;
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto key = reinterpret_cast<std::uint64_t>(resource);
    const auto origin = g_resource_callsites.find(key);
    native_ui = g_native_ui_resources.contains(key) ||
        (origin != g_resource_callsites.end() && is_ui_allocation(origin->second));
    ReleaseSRWLockShared(&g_pipeline_lock);
    resource->Release();
    if (texture == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->Release();
    const int level = screen_level(desc.Width, desc.Height, state->backbuffer_width, state->backbuffer_height);
    if (native_ui || level < 0) return false;
    ID3D11Buffer *original = nullptr;
    context->CSGetConstantBuffers(0, 1, &original);
    if (original == nullptr) return false;
    BufferSnapshot snapshot = {};
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto found = g_buffer_snapshots.find(reinterpret_cast<std::uint64_t>(original));
    if (found != g_buffer_snapshots.end()) snapshot = found->second;
    ReleaseSRWLockShared(&g_pipeline_lock);
    if (snapshot.size < 32) { original->Release(); return false; }
    auto *rect = reinterpret_cast<std::uint32_t *>(snapshot.bytes.data() + 16);
    if (rect[0] == 0 || rect[1] == 0 || rect[2] >= desc.Width || rect[3] >= desc.Height ||
        rect[0] > desc.Width - rect[2] || rect[1] > desc.Height - rect[3]) {
        original->Release(); return false;
    }
    const double sx = static_cast<double>((std::max)(1L, state->render_width >> level)) / desc.Width;
    const double sy = static_cast<double>((std::max)(1L, state->render_height >> level)) / desc.Height;
    const auto x = static_cast<std::uint32_t>(std::floor(rect[2] * sx));
    const auto y = static_cast<std::uint32_t>(std::floor(rect[3] * sy));
    rect[0] = static_cast<std::uint32_t>(std::ceil((static_cast<double>(rect[2]) + rect[0]) * sx)) - x;
    rect[1] = static_cast<std::uint32_t>(std::ceil((static_cast<double>(rect[3]) + rect[1]) * sy)) - y;
    rect[2] = x; rect[3] = y;
    if (g_scaled_clear_constants == nullptr) {
        ID3D11Device *device = nullptr;
        context->GetDevice(&device);
        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = 32; buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const auto result = device->CreateBuffer(&buffer, nullptr, &g_scaled_clear_constants);
        device->Release();
        if (FAILED(result)) { original->Release(); return false; }
    }
    submitting = true;
    context->UpdateSubresource(g_scaled_clear_constants, 0, nullptr, snapshot.bytes.data(), 0, 0);
    context->CSSetConstantBuffers(0, 1, &g_scaled_clear_constants);
    context->Dispatch((rect[0] + 31) / 32, (rect[1] + 31) / 32, 1);
    context->CSSetConstantBuffers(0, 1, &original);
    original->Release();
    submitting = false;
    return true;
}

void on_bind_targets(reshade::api::command_list *cmd, std::uint32_t count,
                     const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
    if (g_internal_render || cmd == nullptr)
        return;
    g_current_screen_level = -1;
    g_current_color_target = 0;
    g_current_native_ui_target = false;
    g_current_gbuffer_target = false;
    g_current_scene_depth_target = false;
    g_current_full_scene_depth_target = false;
    auto *bound_device = cmd->get_device();
    const auto *state = g_mapping.get();
    const auto output_width = state != nullptr ? static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_mapping.get()->backbuffer_width, 0, 0)) : 0;
    const auto output_height = state != nullptr ? static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_mapping.get()->backbuffer_height, 0, 0)) : 0;
    if (bound_device != nullptr && output_width != 0 && output_height != 0)
    {
        reshade::api::resource first_resource = {};
        if (count != 0 && rtvs != nullptr && rtvs[0].handle != 0)
            first_resource = bound_device->get_resource_from_view(rtvs[0]);
        if (first_resource.handle != 0)
        {
            g_current_color_target = first_resource.handle;
            const auto desc = bound_device->get_resource_desc(first_resource);
            g_current_screen_level = screen_level(desc.texture.width, desc.texture.height,
                                                  output_width, output_height);
            AcquireSRWLockShared(&g_pipeline_lock);
            const auto origin = g_resource_callsites.find(first_resource.handle);
            if (origin != g_resource_callsites.end())
                g_current_native_ui_target = is_ui_allocation(origin->second);
            if (!g_current_native_ui_target)
                g_current_native_ui_target =
                    g_native_ui_resources.contains(first_resource.handle);
            ReleaseSRWLockShared(&g_pipeline_lock);
        }
        else if (dsv.handle != 0)
        {
            const auto depth_resource = bound_device->get_resource_from_view(dsv);
            if (depth_resource.handle != 0)
            {
                const auto desc = bound_device->get_resource_desc(depth_resource);
                g_current_screen_level = screen_level(desc.texture.width, desc.texture.height,
                                                      output_width, output_height);
            }
        }
    }
    if (bound_device != nullptr && dsv.handle != 0)
    {
        const auto depth_resource = bound_device->get_resource_from_view(dsv);
        if (depth_resource.handle != 0) {
            const auto depth = bound_device->get_resource_desc(depth_resource);
            // Scaleform shares the screen-sized scene DSV while writing its
            // glyph caches. Only depth-only geometry can open the 3D phase.
            g_current_full_scene_depth_target = count == 0 && depth.texture.width == output_width &&
                depth.texture.height == output_height;
        }
        const auto known_depth = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&g_scene_depth_resource, 0, 0));
        g_current_scene_depth_target = depth_resource.handle != 0 &&
                                       depth_resource.handle == known_depth;
    }
    if (bound_device != nullptr && count == 4 && rtvs != nullptr &&
        rtvs[0].handle != 0 && rtvs[1].handle != 0 && rtvs[2].handle != 0 &&
        rtvs[3].handle != 0 && dsv.handle != 0 && output_width != 0)
    {
        const auto color_resource = bound_device->get_resource_from_view(rtvs[0]);
        const auto second_resource = bound_device->get_resource_from_view(rtvs[1]);
        const auto third_resource = bound_device->get_resource_from_view(rtvs[2]);
        const auto fourth_resource = bound_device->get_resource_from_view(rtvs[3]);
        const auto depth_resource = bound_device->get_resource_from_view(dsv);
        if (color_resource.handle != 0 && second_resource.handle != 0 &&
            third_resource.handle != 0 && fourth_resource.handle != 0 &&
            depth_resource.handle != 0)
        {
            const auto color = bound_device->get_resource_desc(color_resource);
            const auto second = bound_device->get_resource_desc(second_resource);
            const auto third = bound_device->get_resource_desc(third_resource);
            const auto fourth = bound_device->get_resource_desc(fourth_resource);
            g_current_gbuffer_target = color.texture.width == output_width &&
                color.texture.format == reshade::api::format::r11g11b10_float &&
                second.texture.format == reshade::api::format::r16g16_float &&
                third.texture.format == reshade::api::format::r8g8b8a8_unorm &&
                fourth.texture.format == reshade::api::format::r8g8b8a8_unorm;
            if (g_current_gbuffer_target)
            {
                g_scene_render_phase = true;
                InterlockedExchange64(&g_scene_depth_resource,
                                      static_cast<LONG64>(depth_resource.handle));
                g_current_scene_depth_target = true;
                g_frame_pipeline.set_scene_depth(
                    reinterpret_cast<ID3D11Resource *>(depth_resource.handle));
            }
        }
    }
    if (!g_diagnostics)
        return;
    InterlockedIncrement64(&g_frame_target_binds);

    if (g_capture_active)
    {
        if (g_captured_pass_count < static_cast<std::uint32_t>(std::size(g_captured_passes)))
        {
            g_current_captured_pass = static_cast<std::int32_t>(g_captured_pass_count++);
            auto &pass = g_captured_passes[g_current_captured_pass];
            pass = {};
            pass.target_count = count;
            auto *trace_device = cmd->get_device();
            if (trace_device != nullptr && rtvs != nullptr)
            {
                for (std::uint32_t i = 0; i < count && i < 8; ++i)
                {
                    if (rtvs[i].handle == 0)
                        continue;
                    const auto resource = trace_device->get_resource_from_view(rtvs[i]);
                    if (resource.handle == 0)
                        continue;
                    pass.targets[i] = describe_target(trace_device, resource);
                }
            }
            if (trace_device != nullptr && dsv.handle != 0)
            {
                const auto resource = trace_device->get_resource_from_view(dsv);
                if (resource.handle != 0)
                {
                    pass.depth = describe_target(trace_device, resource);
                }
            }
        }
        else
            g_current_captured_pass = -1;
    }
    if (count == 0 || rtvs == nullptr || rtvs[0].handle == 0 || dsv.handle == 0)
        return;

    auto *device = cmd->get_device();
    if (device == nullptr)
        return;
    const auto color_resource = device->get_resource_from_view(rtvs[0]);
    const auto depth_resource = device->get_resource_from_view(dsv);
    if (color_resource.handle == 0 || depth_resource.handle == 0)
        return;

    const auto color = device->get_resource_desc(color_resource);
    const auto depth = device->get_resource_desc(depth_resource);

    if (color.texture.width != depth.texture.width || color.texture.height != depth.texture.height)
        return;

    const LONG old_w = InterlockedCompareExchange(&g_frame_scene_w, 0, 0);
    const LONG old_h = InterlockedCompareExchange(&g_frame_scene_h, 0, 0);
    const std::uint64_t old_area = static_cast<std::uint64_t>(old_w) * static_cast<std::uint64_t>(old_h);
    const std::uint64_t new_area = static_cast<std::uint64_t>(color.texture.width) * color.texture.height;
    if (new_area >= old_area)
    {
        InterlockedExchange(&g_frame_scene_w, static_cast<LONG>(color.texture.width));
        InterlockedExchange(&g_frame_scene_h, static_cast<LONG>(color.texture.height));
        InterlockedExchange(&g_frame_color_format, static_cast<LONG>(color.texture.format));
        InterlockedExchange(&g_frame_depth_format, static_cast<LONG>(depth.texture.format));
    }
}

void on_init_pipeline(reshade::api::device *, reshade::api::pipeline_layout,
                      std::uint32_t count, const reshade::api::pipeline_subobject *subobjects,
                      reshade::api::pipeline pipeline)
{
    if (pipeline.handle == 0 || subobjects == nullptr)
        return;
    PipelineShaders shaders = {};
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto &object = subobjects[i];
        if (object.data == nullptr || object.count == 0)
            continue;
        if (object.type != reshade::api::pipeline_subobject_type::vertex_shader &&
            object.type != reshade::api::pipeline_subobject_type::pixel_shader &&
            object.type != reshade::api::pipeline_subobject_type::compute_shader)
            continue;
        const auto *descs = static_cast<const reshade::api::shader_desc *>(object.data);
        if (descs[0].code == nullptr || descs[0].code_size == 0)
            continue;
        const auto hash = hash_shader(descs[0].code, descs[0].code_size);
        const bool is_pixel = object.type == reshade::api::pipeline_subobject_type::pixel_shader;
        if (object.type == reshade::api::pipeline_subobject_type::compute_shader)
            shaders.compute = hash;
        else if (!is_pixel)
            shaders.vertex = hash;
        else
            shaders.pixel = hash;
        AcquireSRWLockExclusive(&g_pipeline_lock);
        if (!g_shader_blobs.contains(hash))
        {
            ShaderBlob blob;
            const auto *begin = static_cast<const std::uint8_t *>(descs[0].code);
            blob.bytes.assign(begin, begin + descs[0].code_size);
            blob.pixel = is_pixel;
            g_shader_blobs.emplace(hash, std::move(blob));
        }
        ReleaseSRWLockExclusive(&g_pipeline_lock);
    }
    if (shaders.vertex == 0 && shaders.pixel == 0 && shaders.compute == 0)
        return;
    AcquireSRWLockExclusive(&g_pipeline_lock);
    g_pipeline_shaders[pipeline.handle] = shaders;
    ReleaseSRWLockExclusive(&g_pipeline_lock);
}

void on_destroy_pipeline(reshade::api::device *, reshade::api::pipeline pipeline)
{
    AcquireSRWLockExclusive(&g_pipeline_lock);
    g_pipeline_shaders.erase(pipeline.handle);
    ReleaseSRWLockExclusive(&g_pipeline_lock);
}

void on_bind_pipeline(reshade::api::command_list *, reshade::api::pipeline_stage stages,
                      reshade::api::pipeline pipeline)
{
    if (g_internal_render) return;
    PipelineShaders shaders = {};
    AcquireSRWLockShared(&g_pipeline_lock);
    const auto entry = g_pipeline_shaders.find(pipeline.handle);
    if (entry != g_pipeline_shaders.end())
        shaders = entry->second;
    ReleaseSRWLockShared(&g_pipeline_lock);

    const auto stage_bits = static_cast<std::uint32_t>(stages);
    if ((stage_bits & static_cast<std::uint32_t>(reshade::api::pipeline_stage::vertex_shader)) != 0)
        g_bound_vertex_shader = shaders.vertex;
    if ((stage_bits & static_cast<std::uint32_t>(reshade::api::pipeline_stage::pixel_shader)) != 0)
        g_bound_pixel_shader = shaders.pixel;
    if ((stage_bits & static_cast<std::uint32_t>(reshade::api::pipeline_stage::compute_shader)) != 0)
        g_bound_compute_shader = shaders.compute;
}

void on_push_descriptors(reshade::api::command_list *, reshade::api::shader_stage stages,
                         reshade::api::pipeline_layout, std::uint32_t,
                         const reshade::api::descriptor_table_update &update)
{
    if (g_internal_render) return;
    const auto stage_bits = static_cast<std::uint32_t>(stages);
    const bool vertex = (stage_bits & static_cast<std::uint32_t>(
        reshade::api::shader_stage::vertex)) != 0;
    const bool pixel = (stage_bits & static_cast<std::uint32_t>(
        reshade::api::shader_stage::pixel)) != 0;
    if ((!vertex && !pixel) || update.descriptors == nullptr || update.binding >= 16)
        return;
    const std::uint32_t count = (std::min)(update.count, 16u - update.binding);
    if (pixel && update.type == reshade::api::descriptor_type::shader_resource_view)
    {
        if (g_internal_srv_bind)
            return;
        const auto *views = static_cast<const reshade::api::resource_view *>(update.descriptors);
        for (std::uint32_t i = 0; i < count; ++i)
            g_bound_pixel_inputs[update.binding + i] = views[i];
    }
    else if (update.type == reshade::api::descriptor_type::constant_buffer ||
             update.type == reshade::api::descriptor_type::constant_buffer_with_dynamic_offset)
    {
        const auto *buffers = static_cast<const reshade::api::buffer_range *>(update.descriptors);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            // Calls made through the D3D11 context can be reflected back as
            // ReShade events. Do not mistake our temporary override for the
            // constant buffer the game intends to bind on later draws.
            const bool internal_buffer =
                (g_scaled_pixel_viewport_constants != nullptr &&
                 buffers[i].buffer.handle == reinterpret_cast<std::uint64_t>(g_scaled_pixel_viewport_constants)) ||
                (g_scaled_viewport_constants != nullptr &&
                 buffers[i].buffer.handle == reinterpret_cast<std::uint64_t>(
                    g_scaled_viewport_constants)) ||
                (g_jittered_per_view_constants != nullptr &&
                 buffers[i].buffer.handle == reinterpret_cast<std::uint64_t>(
                    g_jittered_per_view_constants));
            if (vertex && !internal_buffer)
                g_bound_vertex_constant_buffers[update.binding + i] = buffers[i];
            if (pixel && !internal_buffer)
                g_bound_pixel_constant_buffers[update.binding + i] = buffers[i];
            if (!internal_buffer && update.binding + i == 12)
                g_camera_override_bound = false;
        }
    }
}

void write_captured_frame(LONG64 frame_number)
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, g_trace_path, L"w") != 0 || file == nullptr)
        return;
    fprintf(file, "DOS2DLSS render-pass capture for frame %lld\n", frame_number);
    for (std::uint32_t index = 0; index < g_captured_pass_count; ++index)
    {
        const auto &pass = g_captured_passes[index];
        fprintf(file, "pass=%u rt_count=%u draws=%u indexed=%u", index + 1,
                pass.target_count, pass.draws, pass.indexed_draws);
        for (std::uint32_t i = 0; i < pass.target_count && i < 8; ++i)
        {
            const auto &target = pass.targets[i];
            if (target.resource == 0)
                fprintf(file, " rt%u=null", i);
            else
                fprintf(file, " rt%u=%llX:%ux%u:f%u@%llX/%llX/%llX/%llX%s%s", i,
                        static_cast<unsigned long long>(target.resource),
                        target.width, target.height, target.format,
                        static_cast<unsigned long long>(target.creation_sites[0]),
                        static_cast<unsigned long long>(target.creation_sites[1]),
                        static_cast<unsigned long long>(target.creation_sites[2]),
                        static_cast<unsigned long long>(target.creation_sites[3]),
                        target.name[0] != '\0' ? ":" : "", target.name);
        }
        if (pass.depth.resource != 0)
            fprintf(file, " ds=%llX:%ux%u:f%u@%llX/%llX/%llX/%llX%s%s",
                    static_cast<unsigned long long>(pass.depth.resource),
                    pass.depth.width, pass.depth.height, pass.depth.format,
                    static_cast<unsigned long long>(pass.depth.creation_sites[0]),
                    static_cast<unsigned long long>(pass.depth.creation_sites[1]),
                    static_cast<unsigned long long>(pass.depth.creation_sites[2]),
                    static_cast<unsigned long long>(pass.depth.creation_sites[3]),
                    pass.depth.name[0] != '\0' ? ":" : "", pass.depth.name);
        if (pass.first_vertex_shader != 0 || pass.first_pixel_shader != 0)
            fprintf(file, " vs=%016llX..%016llX ps=%016llX..%016llX",
                    static_cast<unsigned long long>(pass.first_vertex_shader),
                    static_cast<unsigned long long>(pass.last_vertex_shader),
                    static_cast<unsigned long long>(pass.first_pixel_shader),
                    static_cast<unsigned long long>(pass.last_pixel_shader));
        for (std::uint32_t i = 0; i < std::size(pass.pixel_inputs); ++i)
        {
            const auto &input = pass.pixel_inputs[i];
            if (input.resource != 0)
                fprintf(file, " t%u=%llX:%ux%u:f%u@%llX/%llX/%llX/%llX%s%s", i,
                        static_cast<unsigned long long>(input.resource),
                        input.width, input.height, input.format,
                        static_cast<unsigned long long>(input.creation_sites[0]),
                        static_cast<unsigned long long>(input.creation_sites[1]),
                        static_cast<unsigned long long>(input.creation_sites[2]),
                        static_cast<unsigned long long>(input.creation_sites[3]),
                        input.name[0] != '\0' ? ":" : "", input.name);
        }
        for (std::uint32_t i = 0; i < std::size(pass.pixel_constant_buffers); ++i)
            if (pass.pixel_constant_buffers[i] != 0)
                fprintf(file, " cb%u=%llX", i,
                        static_cast<unsigned long long>(pass.pixel_constant_buffers[i]));
        if (pass.per_view_valid)
            fprintf(file, " pv=%016llX", static_cast<unsigned long long>(
                hash_shader(pass.per_view.data(), pass.per_view.size())));
        if (pass.vertex_constants_valid)
            fprintf(file, " vc=%016llX", static_cast<unsigned long long>(
                hash_shader(pass.vertex_constants.data(), pass.vertex_constants.size())));
        fputc('\n', file);
    }
    std::unordered_set<std::uint64_t> written_vertex_constants;
    for (std::uint32_t index = 0; index < g_captured_pass_count; ++index)
    {
        const auto &pass = g_captured_passes[index];
        if (!pass.vertex_constants_valid)
            continue;
        const auto hash = hash_shader(pass.vertex_constants.data(),
                                      pass.vertex_constants.size());
        if (!written_vertex_constants.insert(hash).second)
            continue;
        const auto *values = reinterpret_cast<const float *>(pass.vertex_constants.data());
        fprintf(file, "vertex_constants=%016llX", static_cast<unsigned long long>(hash));
        for (std::size_t i = 0; i < pass.vertex_constants.size() / sizeof(float); ++i)
            fprintf(file, " %.9g", values[i]);
        fputc('\n', file);
    }
    std::unordered_set<std::uint64_t> written_per_views;
    for (std::uint32_t index = 0; index < g_captured_pass_count; ++index)
    {
        const auto &pass = g_captured_passes[index];
        if (!pass.per_view_valid)
            continue;
        const auto hash = hash_shader(pass.per_view.data(), pass.per_view.size());
        if (!written_per_views.insert(hash).second)
            continue;
        const auto *values = reinterpret_cast<const float *>(pass.per_view.data());
        fprintf(file, "per_view=%016llX", static_cast<unsigned long long>(hash));
        for (std::size_t i = 0; i < pass.per_view.size() / sizeof(float); ++i)
            fprintf(file, " %.9g", values[i]);
        fputc('\n', file);
    }
    fclose(file);

    CreateDirectoryW(g_shader_dir, nullptr);
    std::unordered_set<std::uint64_t> hashes;
    for (std::uint32_t index = 0; index < g_captured_pass_count; ++index)
    {
        const auto &pass = g_captured_passes[index];
        hashes.insert(pass.first_vertex_shader);
        hashes.insert(pass.last_vertex_shader);
        hashes.insert(pass.first_pixel_shader);
        hashes.insert(pass.last_pixel_shader);
    }
    hashes.erase(0);
    for (const auto hash : hashes)
    {
        ShaderBlob blob;
        AcquireSRWLockShared(&g_pipeline_lock);
        const auto entry = g_shader_blobs.find(hash);
        if (entry != g_shader_blobs.end())
            blob = entry->second;
        ReleaseSRWLockShared(&g_pipeline_lock);
        if (blob.bytes.empty())
            continue;

        wchar_t path[MAX_PATH] = {};
        swprintf_s(path, L"%ls\\%ls-%016llX.cso", g_shader_dir,
                   blob.pixel ? L"ps" : L"vs",
                   static_cast<unsigned long long>(hash));
        FILE *shader = nullptr;
        if (_wfopen_s(&shader, path, L"wb") == 0 && shader != nullptr)
        {
            fwrite(blob.bytes.data(), 1, blob.bytes.size(), shader);
            fclose(shader);
        }
    }
}

void on_present(reshade::api::command_queue *queue, reshade::api::swapchain *,
                const reshade::api::rect *, const reshade::api::rect *,
                std::uint32_t, const reshade::api::rect *)
{
    g_scene_render_phase = false;
    auto *state = g_mapping.get();
    if (state == nullptr)
        return;
    // Hidden diagnostic switches permit a controlled legacy/optimized A/B in
    // one process. They are off/default-fast unless explicitly requested.
    static unsigned profile_frames = 0;
    static int profile_mode = -1;
    bool changed = false;
    if ((state->frame_number & 31) == 0) {
        const bool legacy = GetPrivateProfileIntW(L"DOS2DLSS", L"PerfLegacyHooks", 0, g_config_path) != 0;
        const bool profiling = GetPrivateProfileIntW(L"DOS2DLSS", L"ProfilePerformance", 0, g_config_path) != 0;
        changed = legacy != g_perf_legacy || profiling != g_perf_profile;
        if (legacy != g_perf_legacy) g_jittered_per_view_frame = -1;
        g_perf_legacy = legacy; g_perf_profile = profiling;
    }
    g_frame_pipeline.set_profiling(g_perf_profile, g_perf_legacy);
    if (changed || profile_mode != state->quality_mode) {
        g_camera_calls = g_camera_cache_hits = g_camera_cpu_ticks = g_scoped_draws = g_bypassed_draws = 0;
        profile_frames = 0; profile_mode = state->quality_mode;
    }
    if (g_perf_profile && ++profile_frames >= 120) {
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
        log_line("PERF CPU mode=%d legacy=%d frames=%u camera_ms=%.3f camera_calls=%.1f hits=%.1f scoped_draws=%.1f bypassed_draws=%.1f",
            profile_mode, g_perf_legacy ? 1 : 0, profile_frames,
            1000.0 * g_camera_cpu_ticks / static_cast<double>(frequency.QuadPart) / profile_frames,
            static_cast<double>(g_camera_calls)/profile_frames, static_cast<double>(g_camera_cache_hits)/profile_frames,
            static_cast<double>(g_scoped_draws)/profile_frames, static_cast<double>(g_bypassed_draws)/profile_frames);
        g_camera_calls = g_camera_cache_hits = g_camera_cpu_ticks = g_scoped_draws = g_bypassed_draws = 0;
        profile_frames = 0;
    }
    if (g_capture_active)
    {
        write_captured_frame(state->frame_number);
        g_capture_active = false;
        g_current_captured_pass = -1;
        InterlockedExchange(&state->captured_pass_count,
                            static_cast<LONG>(g_captured_pass_count));
        InterlockedExchange(&state->capture_complete, 1);
        log_line("Captured %u render-target passes for frame %lld.",
                 g_captured_pass_count, state->frame_number);
    }
    if (InterlockedExchange(&state->capture_requested, 0) != 0)
    {
        g_captured_pass_count = 0;
        g_current_captured_pass = -1;
        g_capture_active = true;
        InterlockedExchange(&state->capture_complete, 0);
        InterlockedExchange(&state->captured_pass_count, 0);
    }
    if (queue != nullptr)
    {
        auto *context = reinterpret_cast<ID3D11DeviceContext *>(queue->get_native());
        const int mode = static_cast<int>(InterlockedCompareExchange(&state->quality_mode, 0, 0));
        if (mode == static_cast<int>(dos2dlss::QualityMode::off))
        {
            g_frame_pipeline.reset_history();
            InterlockedExchange(&state->native_dlss_active, 0);
            InterlockedExchange(&state->camera_motion_ready, 0);
            InterlockedExchange(&state->camera_jitter_ready, 0);
        }
        InternalRenderScope internal;
        PrivateContextState::Scope pipeline(g_private_context);
        if (pipeline.active()) g_ngx.sync_feature(context, mode,
            static_cast<std::uint32_t>(state->backbuffer_width),
            static_cast<std::uint32_t>(state->backbuffer_height), state, &log_message);
    }
    InterlockedIncrement64(&state->frame_number);
    InterlockedExchange64(&state->draw_calls, InterlockedExchange64(&g_frame_draws, 0));
    InterlockedExchange64(&state->indexed_draw_calls, InterlockedExchange64(&g_frame_indexed_draws, 0));
    InterlockedExchange64(&state->target_bind_calls, InterlockedExchange64(&g_frame_target_binds, 0));
    InterlockedExchange(&state->scene_width, InterlockedExchange(&g_frame_scene_w, 0));
    InterlockedExchange(&state->scene_height, InterlockedExchange(&g_frame_scene_h, 0));
    InterlockedExchange(&state->scene_color_format, InterlockedExchange(&g_frame_color_format, 0));
    InterlockedExchange(&state->scene_depth_format, InterlockedExchange(&g_frame_depth_format, 0));
}

void on_reshade_present(reshade::api::effect_runtime *runtime)
{
    auto *state = g_mapping.get();
    if (runtime != nullptr && state != nullptr &&
        InterlockedExchange(&state->screenshot_requested, 0) != 0)
    {
        InterlockedExchange(&state->screenshot_complete, 0);
        state->screenshot_path[0] = L'\0';
        runtime->save_screenshot("DOS2DLSS");
    }
}

void on_reshade_screenshot(reshade::api::effect_runtime *, const char *path)
{
    auto *state = g_mapping.get();
    if (state == nullptr)
        return;
    if (path != nullptr)
        MultiByteToWideChar(CP_UTF8, 0, path, -1, state->screenshot_path,
                            static_cast<int>(std::size(state->screenshot_path)));
    InterlockedExchange(&state->screenshot_complete, 1);
}

enum : std::size_t
{
    kOrdGetVersion = 3,
    kOrdSameLine = 85,
    kOrdTextUnformatted = 106,
    kOrdSeparatorText = 113,
    kOrdCheckbox = 118,
    kOrdRadioButton2 = 122,
};

struct PanelImGui
{
    const void *pad_a[kOrdGetVersion - 1];
    const char *(*GetVersion)();
    const void *pad_b[kOrdSameLine - kOrdGetVersion - 1];
    void (*SameLine)(float, float);
    const void *pad_c[kOrdTextUnformatted - kOrdSameLine - 1];
    void (*TextUnformatted)(const char *, const char *);
    const void *pad_d[kOrdSeparatorText - kOrdTextUnformatted - 1];
    void (*SeparatorText)(const char *);
    const void *pad_e[kOrdCheckbox - kOrdSeparatorText - 1];
    bool (*Checkbox)(const char *, bool *);
    const void *pad_f[kOrdRadioButton2 - kOrdCheckbox - 1];
    bool (*RadioButton2)(const char *, int *, int);
};

static_assert(offsetof(PanelImGui, GetVersion) == (kOrdGetVersion - 1) * sizeof(void *));
static_assert(offsetof(PanelImGui, SameLine) == (kOrdSameLine - 1) * sizeof(void *));
static_assert(offsetof(PanelImGui, TextUnformatted) == (kOrdTextUnformatted - 1) * sizeof(void *));
static_assert(offsetof(PanelImGui, SeparatorText) == (kOrdSeparatorText - 1) * sizeof(void *));
static_assert(offsetof(PanelImGui, Checkbox) == (kOrdCheckbox - 1) * sizeof(void *));
static_assert(offsetof(PanelImGui, RadioButton2) == (kOrdRadioButton2 - 1) * sizeof(void *));

const PanelImGui *g_ui = nullptr;

void panel_line(const char *format, ...)
{
    if (g_ui == nullptr)
        return;
    char text[640] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    g_ui->TextUnformatted(text, nullptr);
}

const char *mode_name(int mode)
{
    static const char *names[] = { "Off", "DLAA", "Quality", "Balanced", "Performance" };
    return mode >= 0 && mode <= 4 ? names[mode] : "Invalid";
}

void draw_panel(reshade::api::effect_runtime *)
{
    if (g_ui == nullptr)
        return;
    auto *state = g_mapping.get();
    if (state == nullptr)
    {
        panel_line("Shared state is unavailable.");
        return;
    }

    g_ui->SeparatorText("Mode");
    int mode = static_cast<int>(InterlockedCompareExchange(&state->quality_mode, 0, 0));
    const char *names[] = { "Off", "DLAA", "Quality", "Balanced", "Performance" };
    bool changed = false;
    for (int i = 0; i < 5; ++i)
    {
        if (g_ui->RadioButton2(names[i], &mode, i))
            changed = true;
        if (i != 4) g_ui->SameLine(0.0f, -1.0f);
    }
    if (changed)
    {
        InterlockedExchange(&state->quality_mode, mode);
        save_int(L"Mode", mode);
    }
    panel_line("Selected: %s.", mode_name(mode));

    bool reset = false;
    if (g_ui->Checkbox("Reset DLSS history on the next frame", &reset) && reset)
        InterlockedExchange(&state->reset_requested, 1);

    bool capture = false;
    if (g_ui->Checkbox("Capture render passes on the next frame", &capture) && capture)
    {
        InterlockedExchange(&state->capture_complete, 0);
        InterlockedExchange(&state->capture_requested, 1);
    }
    if (state->capture_complete)
        panel_line("Captured %ld passes to dos2-dlss-frame-trace.log.",
                   state->captured_pass_count);

    bool screenshot = false;
    if (g_ui->Checkbox("Save a ReShade screenshot on the next frame", &screenshot) && screenshot)
    {
        InterlockedExchange(&state->screenshot_complete, 0);
        InterlockedExchange(&state->screenshot_requested, 1);
    }
    if (state->screenshot_complete)
        panel_line("Screenshot: %ls", state->screenshot_path);

    g_ui->SeparatorText("Runtime");
    panel_line("Native module: %s", InterlockedCompareExchange(&state->native_ready, 0, 0) ? "loaded" : "not loaded");
    panel_line("Game build: %s (%ls)", InterlockedCompareExchange(&state->exact_game_build, 0, 0) ? "verified" : "not verified", state->game_version);
    panel_line("NVIDIA NGX: %s; DLSS available: %s; feature: %s",
               InterlockedCompareExchange(&state->ngx_initialized, 0, 0) ? "initialized" : "not initialized",
               InterlockedCompareExchange(&state->ngx_available, 0, 0) ? "yes" : "no",
               InterlockedCompareExchange(&state->ngx_feature_created, 0, 0) ? "created" : "not created");
    panel_line("NGX results: init 0x%08lX, capability 0x%08lX, feature 0x%08lX",
               state->ngx_init_result, state->ngx_capability_result, state->ngx_feature_result);
    panel_line("Native evaluation: %s; camera motion: %s; result 0x%08lX; frames %lld",
               state->native_dlss_active ? mode_name(mode) : "waiting",
               state->camera_motion_ready ? "ready" : "waiting",
               state->ngx_evaluate_result, state->ngx_evaluated_frames);
    panel_line("Projection jitter: %s",
               state->camera_jitter_ready ? "active on 3D camera" : "waiting");
    panel_line("Back buffer: %ld x %ld", state->backbuffer_width, state->backbuffer_height);
    panel_line("DLSS contract: %ld x %ld -> %ld x %ld",
               state->render_width, state->render_height,
               state->backbuffer_width, state->backbuffer_height);
    panel_line("Temporal jitter: %.4f, %.4f render pixels",
               state->jitter_x, state->jitter_y);
    panel_line("Largest color+depth pass this frame: %ld x %ld", state->scene_width, state->scene_height);

    if (g_ui->Checkbox("Collect per-draw diagnostics", &g_diagnostics))
        save_int(L"Diagnostics", g_diagnostics ? 1 : 0);
    if (g_diagnostics)
    {
        panel_line("Frame %lld: %lld draws + %lld indexed, %lld target binds",
                   state->frame_number, state->draw_calls, state->indexed_draw_calls,
                   state->target_bind_calls);
        panel_line("Candidate formats: color %ld, depth %ld",
                   state->scene_color_format, state->scene_depth_format);
    }

    g_ui->SeparatorText("Pipeline");
    panel_line("DOS2 native data -> NVIDIA NGX D3D11 -> native CombineUI");
    panel_line("DLSS 5 Bridge is optional and currently paused for native-DLSS testing.");
    panel_line("Feeder is not used.");
}

bool register_callbacks()
{
    auto reg = reinterpret_cast<RegisterEventFn>(GetProcAddress(g_reshade, "ReShadeRegisterEvent"));
    auto reg_overlay = reinterpret_cast<RegisterOverlayFn>(GetProcAddress(g_reshade, "ReShadeRegisterOverlay"));
    auto get_table = reinterpret_cast<GetImGuiTableFn>(GetProcAddress(g_reshade, "ReShadeGetImGuiFunctionTable"));
    if (reg == nullptr || reg_overlay == nullptr || get_table == nullptr)
        return false;

    reg(reshade::addon_event::init_device, reinterpret_cast<void *>(&on_init_device));
    reg(reshade::addon_event::destroy_device, reinterpret_cast<void *>(&on_destroy_device));
    reg(reshade::addon_event::init_swapchain, reinterpret_cast<void *>(&on_init_swapchain));
    reg(reshade::addon_event::create_resource, reinterpret_cast<void *>(&on_create_resource));
    reg(reshade::addon_event::init_resource, reinterpret_cast<void *>(&on_init_resource));
    reg(reshade::addon_event::destroy_resource, reinterpret_cast<void *>(&on_destroy_resource));
    reg(reshade::addon_event::map_buffer_region, reinterpret_cast<void *>(&on_map_buffer_region));
    reg(reshade::addon_event::unmap_buffer_region, reinterpret_cast<void *>(&on_unmap_buffer_region));
    reg(reshade::addon_event::update_buffer_region, reinterpret_cast<void *>(&on_update_buffer_region));
    reg(reshade::addon_event::init_pipeline, reinterpret_cast<void *>(&on_init_pipeline));
    reg(reshade::addon_event::destroy_pipeline, reinterpret_cast<void *>(&on_destroy_pipeline));
    reg(reshade::addon_event::bind_pipeline, reinterpret_cast<void *>(&on_bind_pipeline));
    reg(reshade::addon_event::push_descriptors, reinterpret_cast<void *>(&on_push_descriptors));
    reg(reshade::addon_event::draw, reinterpret_cast<void *>(&on_draw));
    reg(reshade::addon_event::draw_indexed, reinterpret_cast<void *>(&on_draw_indexed));
    reg(reshade::addon_event::dispatch, reinterpret_cast<void *>(&on_dispatch));
    reg(reshade::addon_event::bind_render_targets_and_depth_stencil, reinterpret_cast<void *>(&on_bind_targets));
    reg(reshade::addon_event::present, reinterpret_cast<void *>(&on_present));
    reg(reshade::addon_event::reshade_present, reinterpret_cast<void *>(&on_reshade_present));
    reg(reshade::addon_event::reshade_screenshot, reinterpret_cast<void *>(&on_reshade_screenshot));

    // The member ordinals above describe the 19000 compatibility table.
    // Requesting the newer 19250 table with this layout reached the wrong
    // TextUnformatted entry and crashed as soon as the panel was opened.
    g_ui = static_cast<const PanelImGui *>(get_table(19000));
    if (g_ui == nullptr || g_ui->GetVersion == nullptr)
        return false;
    const char *imgui_version = g_ui->GetVersion();
    if (imgui_version == nullptr || imgui_version[0] < '0' || imgui_version[0] > '9')
        return false;
    log_line("ReShade ImGui compatibility table 19000, runtime %s.", imgui_version);
    reg_overlay("DOS2 DLSS", &draw_panel);
    return true;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        make_sibling_path(module, L"dos2-dlss.ini", g_config_path);
        make_sibling_path(module, L"dos2-dlss.log", g_log_path);
        make_sibling_path(module, L"dos2-dlss-frame-trace.log", g_trace_path);
        make_sibling_path(module, L"DOS2DLSS-Shaders", g_shader_dir);
        make_sibling_path(module, L"DOS2DLSS-NGX", g_ngx_path);
        MODULEINFO exe_info = {};
        if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr),
                                 &exe_info, sizeof(exe_info)))
        {
            g_exe_base = reinterpret_cast<std::uintptr_t>(exe_info.lpBaseOfDll);
            g_exe_end = g_exe_base + exe_info.SizeOfImage;
        }
        FILE *clear = nullptr;
        if (_wfopen_s(&clear, g_log_path, L"w") == 0 && clear != nullptr) fclose(clear);

        if (!g_mapping.open_or_create())
            return FALSE;
        auto *state = g_mapping.get();
        InterlockedExchange(&state->quality_mode, load_mode());
        g_diagnostics = GetPrivateProfileIntW(L"DOS2DLSS", L"Diagnostics", 0, g_config_path) != 0;

        if (!register_with_reshade() || !register_callbacks())
        {
            log_line("Registration with ReShade 6.8 add-on API failed.");
            return FALSE;
        }
        InterlockedExchange(&state->addon_ready, 1);
        update_status(L"ReShade probe panel registered.");
        log_line("DOS2 DLSS 0.6.2-dev registered with ReShade.");
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        if (auto *state = g_mapping.get())
            InterlockedExchange(&state->addon_ready, 0);
        if (g_unregister != nullptr)
            g_unregister(g_self);
        g_mapping.close();
    }
    return TRUE;
}
