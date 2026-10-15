#include "voice_ptt.h"

#ifdef _WIN32

#include "engine.h"
#include "common/cvar.h"

#include <cvars/cvar_defaults.h>

#include <Windows.h>

#include <cctype>

namespace
{
    // The engine drops held keys when it loses focus, so a held push-to-talk has to re-assert.
    constexpr double kReassertSeconds = 0.5;

    goldsrc::cvar_t* g_ptt_enabled = nullptr;
    goldsrc::cvar_t* g_ptt_key = nullptr;

    bool g_held = false;
    double g_last_assert = 0.0;

    int KeyNameToVirtualKey(const char* name)
    {
        if (name == nullptr || name[0] == '\0')
        {
            return 0;
        }

        // Left and right mouse are deliberately absent: a stray click in any window would transmit.
        if (_stricmp(name, "mouse3") == 0) return VK_MBUTTON;
        if (_stricmp(name, "mouse4") == 0) return VK_XBUTTON1;
        if (_stricmp(name, "mouse5") == 0) return VK_XBUTTON2;
        if (_stricmp(name, "space") == 0) return VK_SPACE;
        if (_stricmp(name, "shift") == 0) return VK_SHIFT;
        if (_stricmp(name, "ctrl") == 0) return VK_CONTROL;
        if (_stricmp(name, "alt") == 0) return VK_MENU;
        if (_stricmp(name, "tab") == 0) return VK_TAB;

        if (name[1] == '\0' && std::isalnum(static_cast<unsigned char>(name[0])))
        {
            return std::toupper(static_cast<unsigned char>(name[0]));
        }

        return 0;
    }

    // Compared by owning process rather than by window handle: the game may own several windows.
    bool IsGameFocused()
    {
        HWND foreground = GetForegroundWindow();

        if (foreground == nullptr)
        {
            return false;
        }

        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(foreground, &foreground_pid);

        return foreground_pid == GetCurrentProcessId();
    }

    void Release()
    {
        if (!g_held)
        {
            return;
        }

        gEngfuncs.pfnClientCmd("-voicerecord");
        g_held = false;
    }
} // namespace

void VoicePtt_Init()
{
    g_ptt_enabled = gEngfuncs.pfnRegisterVariable(cvars::kVoiceBackgroundPtt.name, cvars::kVoiceBackgroundPtt.value, goldsrc::FCVAR_ARCHIVE);
    g_ptt_key = gEngfuncs.pfnRegisterVariable(cvars::kVoiceBackgroundPttKey.name, cvars::kVoiceBackgroundPttKey.value, goldsrc::FCVAR_ARCHIVE);
}

void VoicePtt_Frame()
{
    // Focused play keeps the engine's own binding, and nothing is transmitted outside a server.
    if (g_ptt_enabled == nullptr || g_ptt_enabled->value == 0.0f ||
        cls == nullptr || cls->state != goldsrc::ca_active || IsGameFocused())
    {
        Release();
        return;
    }

    const char* key_name = (g_ptt_key != nullptr && g_ptt_key->string != nullptr) ? g_ptt_key->string : "";
    int key = KeyNameToVirtualKey(key_name);

    if (key == 0 || (GetAsyncKeyState(key) & 0x8000) == 0)
    {
        Release();
        return;
    }

    double now = realtime != nullptr ? *realtime : 0.0;

    if (!g_held || now - g_last_assert >= kReassertSeconds)
    {
        gEngfuncs.pfnClientCmd("+voicerecord");
        g_held = true;
        g_last_assert = now;
    }
}

#else

void VoicePtt_Init()
{
}

void VoicePtt_Frame()
{
}

#endif
