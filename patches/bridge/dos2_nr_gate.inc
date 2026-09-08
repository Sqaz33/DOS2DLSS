// DOS2-specific gate: leave native DLSS alone while RenoDX NR is disabled.
// Reads ReShade's in-memory setting, not the periodically saved INI file.
using Dos2GetConfig = bool (*)(void *, void *, const char *, const char *, char *, size_t *);
static Dos2GetConfig g_dos2_get_config = nullptr;
static thread_local bool g_dos2_reset_native = true;

static bool Dos2NrEnabled()
{
    thread_local ULONGLONG checked = 0;
    thread_local bool enabled = false;
    const ULONGLONG now = GetTickCount64();
    if (checked != 0 && now - checked < 100) return enabled;
    checked = now;
    char value[32] = {};
    size_t size = sizeof(value);
    const HMODULE nr = GetModuleHandleW(L"renodx-dlss5.addon64");
    const bool next = nr != nullptr && g_dos2_get_config != nullptr &&
        g_dos2_get_config(nr, nullptr,
                          "RenoDX.DLSS5", "NeuralUplift", value, &size) && atof(value) > 0.0;
    static LONG reported = -1;
    if (InterlockedExchange(&reported, next ? 1 : 0) != (next ? 1 : 0))
    {
        EnterCriticalSection(&g_bridge_cs);
        // Each engine must restart its history after handing over to the other.
        g_bridge.frame_ready = false;
        g_bridge.need_reset = true;
        LeaveCriticalSection(&g_bridge_cs);
        Log("[DOS2 NR gate] %s", next ? "NR on: mirror enabled" : "NR off/unavailable: native DLSS only; mirror bypassed");
    }
    if (enabled && !next) g_dos2_reset_native = true;
    enabled = next;
    return enabled;
}
