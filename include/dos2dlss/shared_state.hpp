#pragma once

#include <windows.h>
#include <cstdint>

namespace dos2dlss
{
inline constexpr wchar_t kSharedMappingName[] = L"Local\\DOS2DLSS_State_v5";
inline constexpr std::uint32_t kSharedMagic = 0x35534C44; // "DLS5"
inline constexpr std::uint32_t kSharedVersion = 7;

enum class QualityMode : LONG
{
    off = 0,
    dlaa = 1,
    quality = 2,
    balanced = 3,
    performance = 4,
};

struct SharedState
{
    std::uint32_t magic;
    std::uint32_t version;
    volatile LONG native_ready;
    volatile LONG addon_ready;
    volatile LONG exact_game_build;
    volatile LONG quality_mode;
    volatile LONG reset_requested;
    volatile LONG capture_requested;
    volatile LONG capture_complete;
    volatile LONG captured_pass_count;
    volatile LONG screenshot_requested;
    volatile LONG screenshot_complete;
    volatile LONG ngx_initialized;
    volatile LONG ngx_available;
    volatile LONG ngx_feature_created;
    volatile LONG ngx_init_result;
    volatile LONG ngx_capability_result;
    volatile LONG ngx_feature_result;
    volatile LONG ngx_evaluate_result;
    volatile LONG camera_motion_ready;
    volatile LONG camera_jitter_ready;
    volatile LONG native_dlss_active;
    volatile LONG64 frame_number;
    volatile LONG64 draw_calls;
    volatile LONG64 indexed_draw_calls;
    volatile LONG64 target_bind_calls;
    volatile LONG64 ngx_evaluated_frames;
    volatile LONG backbuffer_width;
    volatile LONG backbuffer_height;
    volatile LONG scene_width;
    volatile LONG scene_height;
    volatile LONG scene_color_format;
    volatile LONG scene_depth_format;
    volatile LONG render_width;
    volatile LONG render_height;
    float jitter_x;
    float jitter_y;
    wchar_t game_version[32];
    wchar_t game_sha256[65];
    wchar_t native_status[192];
    wchar_t addon_status[192];
    wchar_t screenshot_path[MAX_PATH];
};

class SharedMapping
{
public:
    bool open_or_create()
    {
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(sizeof(SharedState)), kSharedMappingName);
        if (mapping_ == nullptr)
            return false;

        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        state_ = static_cast<SharedState *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (state_ == nullptr)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        if (created || state_->magic != kSharedMagic || state_->version != kSharedVersion)
        {
            ZeroMemory(state_, sizeof(*state_));
            state_->magic = kSharedMagic;
            state_->version = kSharedVersion;
            state_->quality_mode = static_cast<LONG>(QualityMode::quality);
        }
        return true;
    }

    void close()
    {
        if (state_ != nullptr)
        {
            UnmapViewOfFile(state_);
            state_ = nullptr;
        }
        if (mapping_ != nullptr)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    SharedState *get() const { return state_; }

private:
    HANDLE mapping_ = nullptr;
    SharedState *state_ = nullptr;
};
}
