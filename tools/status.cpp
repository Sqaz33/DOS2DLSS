#include "dos2dlss/shared_state.hpp"

#include <windows.h>
#include <cstdio>

int wmain()
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, dos2dlss::kSharedMappingName);
    if (mapping == nullptr)
    {
        fwprintf(stderr, L"DOS2DLSS is not running.\n");
        return 1;
    }
    const auto *state = static_cast<const dos2dlss::SharedState *>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(dos2dlss::SharedState)));
    if (state == nullptr)
    {
        CloseHandle(mapping);
        fwprintf(stderr, L"Cannot read DOS2DLSS shared state.\n");
        return 2;
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
    wprintf(L"frame=%lld\n", state->frame_number);
    wprintf(L"draws=%lld\n", state->draw_calls);
    wprintf(L"indexed_draws=%lld\n", state->indexed_draw_calls);
    wprintf(L"target_binds=%lld\n", state->target_bind_calls);
    wprintf(L"backbuffer=%ldx%ld\n", state->backbuffer_width, state->backbuffer_height);
    wprintf(L"scene_candidate=%ldx%ld\n", state->scene_width, state->scene_height);
    wprintf(L"dlss_render_size=%ldx%ld\n", state->render_width, state->render_height);
    wprintf(L"color_format=%ld\n", state->scene_color_format);
    wprintf(L"depth_format=%ld\n", state->scene_depth_format);
    wprintf(L"native_status=%ls\n", state->native_status);
    wprintf(L"addon_status=%ls\n", state->addon_status);

    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return 0;
}
