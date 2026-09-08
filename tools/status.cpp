#include "dos2dlss/shared_state.hpp"

#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    const bool request_capture = argc == 2 && _wcsicmp(argv[1], L"--capture") == 0;
    const bool request_screenshot = argc == 2 && _wcsicmp(argv[1], L"--screenshot") == 0;
    const DWORD access = (request_capture || request_screenshot) ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ;
    HANDLE mapping = OpenFileMappingW(access, FALSE, dos2dlss::kSharedMappingName);
    if (mapping == nullptr)
    {
        fwprintf(stderr, L"DOS2DLSS is not running.\n");
        return 1;
    }
    auto *state = static_cast<dos2dlss::SharedState *>(
        MapViewOfFile(mapping, access, 0, 0, sizeof(dos2dlss::SharedState)));
    if (state == nullptr)
    {
        CloseHandle(mapping);
        fwprintf(stderr, L"Cannot read DOS2DLSS shared state.\n");
        return 2;
    }

    if (request_capture)
    {
        InterlockedExchange(&state->capture_complete, 0);
        InterlockedExchange(&state->capture_requested, 1);
        wprintf(L"Render-pass capture requested.\n");
    }
    if (request_screenshot)
    {
        InterlockedExchange(&state->screenshot_complete, 0);
        InterlockedExchange(&state->screenshot_requested, 1);
        wprintf(L"ReShade screenshot requested.\n");
    }

    wprintf(L"native_ready=%ld\n", state->native_ready);
    wprintf(L"addon_ready=%ld\n", state->addon_ready);
    wprintf(L"exact_game_build=%ld\n", state->exact_game_build);
    wprintf(L"game_version=%ls\n", state->game_version);
    wprintf(L"quality_mode=%ld\n", state->quality_mode);
    wprintf(L"ngx_initialized=%ld\n", state->ngx_initialized);
    wprintf(L"ngx_available=%ld\n", state->ngx_available);
    wprintf(L"ngx_feature_created=%ld\n", state->ngx_feature_created);
    wprintf(L"ngx_init_result=0x%08lX\n", state->ngx_init_result);
    wprintf(L"ngx_capability_result=0x%08lX\n", state->ngx_capability_result);
    wprintf(L"ngx_feature_result=0x%08lX\n", state->ngx_feature_result);
    wprintf(L"ngx_evaluate_result=0x%08lX\n", state->ngx_evaluate_result);
    wprintf(L"camera_motion_ready=%ld\n", state->camera_motion_ready);
    wprintf(L"native_dlss_active=%ld\n", state->native_dlss_active);
    wprintf(L"ngx_evaluated_frames=%lld\n", state->ngx_evaluated_frames);
    wprintf(L"frame=%lld\n", state->frame_number);
    wprintf(L"draws=%lld\n", state->draw_calls);
    wprintf(L"indexed_draws=%lld\n", state->indexed_draw_calls);
    wprintf(L"target_binds=%lld\n", state->target_bind_calls);
    wprintf(L"backbuffer=%ldx%ld\n", state->backbuffer_width, state->backbuffer_height);
    wprintf(L"scene_candidate=%ldx%ld\n", state->scene_width, state->scene_height);
    wprintf(L"dlss_render_size=%ldx%ld\n", state->render_width, state->render_height);
    wprintf(L"capture_requested=%ld\n", state->capture_requested);
    wprintf(L"capture_complete=%ld\n", state->capture_complete);
    wprintf(L"captured_pass_count=%ld\n", state->captured_pass_count);
    wprintf(L"screenshot_requested=%ld\n", state->screenshot_requested);
    wprintf(L"screenshot_complete=%ld\n", state->screenshot_complete);
    wprintf(L"screenshot_path=%ls\n", state->screenshot_path);
    wprintf(L"color_format=%ld\n", state->scene_color_format);
    wprintf(L"depth_format=%ld\n", state->scene_depth_format);
    wprintf(L"native_status=%ls\n", state->native_status);
    wprintf(L"addon_status=%ls\n", state->addon_status);

    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return 0;
}
