#include "dos2dlss/shared_state.hpp"
#include "ngx_runtime.hpp"

#include <windows.h>
#include <psapi.h>
#include <d3d11.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>

#include "reshade_api.hpp"
#include "reshade_events.hpp"

extern "C" __declspec(dllexport) const char *NAME = "DOS2 DLSS 0.2.0";
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
wchar_t g_ngx_path[MAX_PATH] = {};
dos2dlss::NgxRuntime g_ngx;
volatile LONG64 g_frame_draws = 0;
volatile LONG64 g_frame_indexed_draws = 0;
volatile LONG64 g_frame_target_binds = 0;
volatile LONG g_frame_scene_w = 0;
volatile LONG g_frame_scene_h = 0;
volatile LONG g_frame_color_format = 0;
volatile LONG g_frame_depth_format = 0;
bool g_diagnostics = true;

struct CapturedTarget
{
    std::uint64_t resource = 0;
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
};

struct PipelineShaders
{
    std::uint64_t vertex = 0;
    std::uint64_t pixel = 0;
};

CapturedPass g_captured_passes[256] = {};
std::uint32_t g_captured_pass_count = 0;
std::int32_t g_current_captured_pass = -1;
bool g_capture_active = false;
SRWLOCK g_pipeline_lock = SRWLOCK_INIT;
std::unordered_map<std::uint64_t, PipelineShaders> g_pipeline_shaders;
thread_local std::uint64_t g_bound_vertex_shader = 0;
thread_local std::uint64_t g_bound_pixel_shader = 0;

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
    read_debug_name(resource.handle, target.name);
    return target;
}

void record_bound_shaders()
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
        if (g_ngx.initialize(native_device, g_ngx_path, g_mapping.get(), &log_message))
            update_status(L"D3D11 active; NVIDIA NGX initialized.");
        else
            update_status(L"D3D11 active; NVIDIA NGX initialization failed. See dos2-dlss.log.");
    }
    else
        update_status(L"Unsupported graphics API: DOS2DLSS requires D3D11.");
}

void on_destroy_device(reshade::api::device *)
{
    g_ngx.shutdown(g_mapping.get(), &log_message);
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

bool on_draw(reshade::api::command_list *, std::uint32_t, std::uint32_t,
             std::uint32_t, std::uint32_t)
{
    if (g_diagnostics)
        InterlockedIncrement64(&g_frame_draws);
    if (g_capture_active && g_current_captured_pass >= 0)
    {
        ++g_captured_passes[g_current_captured_pass].draws;
        record_bound_shaders();
    }
    return false;
}

bool on_draw_indexed(reshade::api::command_list *, std::uint32_t, std::uint32_t,
                     std::uint32_t, std::int32_t, std::uint32_t)
{
    if (g_diagnostics)
        InterlockedIncrement64(&g_frame_indexed_draws);
    if (g_capture_active && g_current_captured_pass >= 0)
    {
        ++g_captured_passes[g_current_captured_pass].indexed_draws;
        record_bound_shaders();
    }
    return false;
}

void on_bind_targets(reshade::api::command_list *cmd, std::uint32_t count,
                     const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
    if (!g_diagnostics || cmd == nullptr)
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
            object.type != reshade::api::pipeline_subobject_type::pixel_shader)
            continue;
        const auto *descs = static_cast<const reshade::api::shader_desc *>(object.data);
        if (descs[0].code == nullptr || descs[0].code_size == 0)
            continue;
        const auto hash = hash_shader(descs[0].code, descs[0].code_size);
        if (object.type == reshade::api::pipeline_subobject_type::vertex_shader)
            shaders.vertex = hash;
        else
            shaders.pixel = hash;
    }
    if (shaders.vertex == 0 && shaders.pixel == 0)
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
                fprintf(file, " rt%u=%llX:%ux%u:f%u%s%s", i,
                        static_cast<unsigned long long>(target.resource),
                        target.width, target.height, target.format,
                        target.name[0] != '\0' ? ":" : "", target.name);
        }
        if (pass.depth.resource != 0)
            fprintf(file, " ds=%llX:%ux%u:f%u%s%s",
                    static_cast<unsigned long long>(pass.depth.resource),
                    pass.depth.width, pass.depth.height, pass.depth.format,
                    pass.depth.name[0] != '\0' ? ":" : "", pass.depth.name);
        if (pass.first_vertex_shader != 0 || pass.first_pixel_shader != 0)
            fprintf(file, " vs=%016llX..%016llX ps=%016llX..%016llX",
                    static_cast<unsigned long long>(pass.first_vertex_shader),
                    static_cast<unsigned long long>(pass.last_vertex_shader),
                    static_cast<unsigned long long>(pass.first_pixel_shader),
                    static_cast<unsigned long long>(pass.last_pixel_shader));
        fputc('\n', file);
    }
    fclose(file);
}

void on_present(reshade::api::command_queue *queue, reshade::api::swapchain *,
                const reshade::api::rect *, const reshade::api::rect *,
                std::uint32_t, const reshade::api::rect *)
{
    auto *state = g_mapping.get();
    if (state == nullptr)
        return;
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
        g_ngx.sync_feature(context, mode,
                           static_cast<std::uint32_t>(state->backbuffer_width),
                           static_cast<std::uint32_t>(state->backbuffer_height),
                           state, &log_message);
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

    g_ui->SeparatorText("Runtime");
    panel_line("Native module: %s", InterlockedCompareExchange(&state->native_ready, 0, 0) ? "loaded" : "not loaded");
    panel_line("Game build: %s (%ls)", InterlockedCompareExchange(&state->exact_game_build, 0, 0) ? "verified" : "not verified", state->game_version);
    panel_line("NVIDIA NGX: %s; DLSS available: %s; feature: %s",
               InterlockedCompareExchange(&state->ngx_initialized, 0, 0) ? "initialized" : "not initialized",
               InterlockedCompareExchange(&state->ngx_available, 0, 0) ? "yes" : "no",
               InterlockedCompareExchange(&state->ngx_feature_created, 0, 0) ? "created" : "not created");
    panel_line("NGX results: init 0x%08lX, capability 0x%08lX, feature 0x%08lX",
               state->ngx_init_result, state->ngx_capability_result, state->ngx_feature_result);
    panel_line("Back buffer: %ld x %ld", state->backbuffer_width, state->backbuffer_height);
    panel_line("DLSS contract: %ld x %ld -> %ld x %ld",
               state->render_width, state->render_height,
               state->backbuffer_width, state->backbuffer_height);
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
    panel_line("DOS2 native data -> NGX D3D11 -> DLSS 5 Bridge -> RenoDX DLSS 5");
    panel_line("Feeder is not used. Bridge synthesis must be disabled.");
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
    reg(reshade::addon_event::init_pipeline, reinterpret_cast<void *>(&on_init_pipeline));
    reg(reshade::addon_event::destroy_pipeline, reinterpret_cast<void *>(&on_destroy_pipeline));
    reg(reshade::addon_event::bind_pipeline, reinterpret_cast<void *>(&on_bind_pipeline));
    reg(reshade::addon_event::draw, reinterpret_cast<void *>(&on_draw));
    reg(reshade::addon_event::draw_indexed, reinterpret_cast<void *>(&on_draw_indexed));
    reg(reshade::addon_event::bind_render_targets_and_depth_stencil, reinterpret_cast<void *>(&on_bind_targets));
    reg(reshade::addon_event::present, reinterpret_cast<void *>(&on_present));

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
        make_sibling_path(module, L"DOS2DLSS-NGX", g_ngx_path);
        FILE *clear = nullptr;
        if (_wfopen_s(&clear, g_log_path, L"w") == 0 && clear != nullptr) fclose(clear);

        if (!g_mapping.open_or_create())
            return FALSE;
        auto *state = g_mapping.get();
        InterlockedExchange(&state->quality_mode, load_mode());
        g_diagnostics = GetPrivateProfileIntW(L"DOS2DLSS", L"Diagnostics", 1, g_config_path) != 0;

        if (!register_with_reshade() || !register_callbacks())
        {
            log_line("Registration with ReShade 6.8 add-on API failed.");
            return FALSE;
        }
        InterlockedExchange(&state->addon_ready, 1);
        update_status(L"ReShade probe panel registered.");
        log_line("DOS2 DLSS 0.2.0 registered with ReShade.");
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
