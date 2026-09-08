#include "dos2dlss/shared_state.hpp"

#include <windows.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "reshade_api.hpp"
#include "reshade_events.hpp"

extern "C" __declspec(dllexport) const char *NAME = "DOS2 DLSS 0.1.0";
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
volatile LONG64 g_frame_draws = 0;
volatile LONG64 g_frame_indexed_draws = 0;
volatile LONG64 g_frame_target_binds = 0;
volatile LONG g_frame_scene_w = 0;
volatile LONG g_frame_scene_h = 0;
volatile LONG g_frame_color_format = 0;
volatile LONG g_frame_depth_format = 0;
bool g_diagnostics = true;
LONG64 g_trace_frame = 120;
volatile LONG g_trace_bind_index = 0;

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
        update_status(L"D3D11 probe active; NGX feature is not created in milestone 0.1.0.");
    else
        update_status(L"Unsupported graphics API: DOS2DLSS requires D3D11.");
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
    return false;
}

bool on_draw_indexed(reshade::api::command_list *, std::uint32_t, std::uint32_t,
                     std::uint32_t, std::int32_t, std::uint32_t)
{
    if (g_diagnostics)
        InterlockedIncrement64(&g_frame_indexed_draws);
    return false;
}

void on_bind_targets(reshade::api::command_list *cmd, std::uint32_t count,
                     const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
    if (!g_diagnostics || cmd == nullptr)
        return;
    InterlockedIncrement64(&g_frame_target_binds);

    auto *shared = g_mapping.get();
    if (shared != nullptr && shared->frame_number == g_trace_frame)
    {
        const LONG bind_index = InterlockedIncrement(&g_trace_bind_index);
        char line[1024] = {};
        int offset = _snprintf_s(line, sizeof(line), _TRUNCATE, "TRACE bind=%ld rt_count=%u", bind_index, count);
        auto *trace_device = cmd->get_device();
        if (trace_device != nullptr && rtvs != nullptr)
        {
            for (std::uint32_t i = 0; i < count && i < 8 && offset > 0 && offset < static_cast<int>(sizeof(line)); ++i)
            {
                if (rtvs[i].handle == 0)
                {
                    offset += _snprintf_s(line + offset, sizeof(line) - offset, _TRUNCATE, " rt%u=null", i);
                    continue;
                }
                const auto resource = trace_device->get_resource_from_view(rtvs[i]);
                if (resource.handle == 0)
                {
                    offset += _snprintf_s(line + offset, sizeof(line) - offset, _TRUNCATE, " rt%u=null", i);
                    continue;
                }
                const auto desc = trace_device->get_resource_desc(resource);
                offset += _snprintf_s(line + offset, sizeof(line) - offset, _TRUNCATE,
                                      " rt%u=%llX:%ux%u:f%u", i,
                                      static_cast<unsigned long long>(resource.handle),
                                      desc.texture.width, desc.texture.height,
                                      static_cast<unsigned>(desc.texture.format));
            }
        }
        if (trace_device != nullptr && dsv.handle != 0 && offset > 0 && offset < static_cast<int>(sizeof(line)))
        {
            const auto resource = trace_device->get_resource_from_view(dsv);
            if (resource.handle != 0)
            {
                const auto desc = trace_device->get_resource_desc(resource);
                _snprintf_s(line + offset, sizeof(line) - offset, _TRUNCATE,
                            " ds=%llX:%ux%u:f%u", static_cast<unsigned long long>(resource.handle),
                            desc.texture.width, desc.texture.height,
                            static_cast<unsigned>(desc.texture.format));
            }
        }
        log_line("%s", line);
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

void on_present(reshade::api::command_queue *, reshade::api::swapchain *,
                const reshade::api::rect *, const reshade::api::rect *,
                std::uint32_t, const reshade::api::rect *)
{
    auto *state = g_mapping.get();
    if (state == nullptr)
        return;
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
    panel_line("Selected: %s. Probe 0.1.0 does not run NGX yet.", mode_name(mode));

    bool reset = false;
    if (g_ui->Checkbox("Reset DLSS history on the next frame", &reset) && reset)
        InterlockedExchange(&state->reset_requested, 1);

    g_ui->SeparatorText("Runtime");
    panel_line("Native module: %s", InterlockedCompareExchange(&state->native_ready, 0, 0) ? "loaded" : "not loaded");
    panel_line("Game build: %s (%ls)", InterlockedCompareExchange(&state->exact_game_build, 0, 0) ? "verified" : "not verified", state->game_version);
    panel_line("ReShade add-on: loaded; NGX feature: %s",
               InterlockedCompareExchange(&state->ngx_feature_created, 0, 0) ? "created" : "not created");
    panel_line("Back buffer: %ld x %ld", state->backbuffer_width, state->backbuffer_height);
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
    reg(reshade::addon_event::init_swapchain, reinterpret_cast<void *>(&on_init_swapchain));
    reg(reshade::addon_event::draw, reinterpret_cast<void *>(&on_draw));
    reg(reshade::addon_event::draw_indexed, reinterpret_cast<void *>(&on_draw_indexed));
    reg(reshade::addon_event::bind_render_targets_and_depth_stencil, reinterpret_cast<void *>(&on_bind_targets));
    reg(reshade::addon_event::present, reinterpret_cast<void *>(&on_present));

    g_ui = static_cast<const PanelImGui *>(get_table(19250));
    if (g_ui == nullptr)
        g_ui = static_cast<const PanelImGui *>(get_table(19000));
    if (g_ui == nullptr || g_ui->GetVersion == nullptr)
        return false;
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
        FILE *clear = nullptr;
        if (_wfopen_s(&clear, g_log_path, L"w") == 0 && clear != nullptr) fclose(clear);

        if (!g_mapping.open_or_create())
            return FALSE;
        auto *state = g_mapping.get();
        InterlockedExchange(&state->quality_mode, load_mode());
        g_diagnostics = GetPrivateProfileIntW(L"DOS2DLSS", L"Diagnostics", 1, g_config_path) != 0;
        g_trace_frame = GetPrivateProfileIntW(L"DOS2DLSS", L"TraceFrame", 120, g_config_path);

        if (!register_with_reshade() || !register_callbacks())
        {
            log_line("Registration with ReShade 6.8 add-on API failed.");
            return FALSE;
        }
        InterlockedExchange(&state->addon_ready, 1);
        update_status(L"ReShade probe panel registered.");
        log_line("DOS2 DLSS 0.1.0 registered with ReShade.");
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
