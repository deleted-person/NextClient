#include "voice_ptt.h"

#ifdef _WIN32

#include "engine.h"
#include "common/cvar.h"

#include <cvars/cvar_defaults.h>

#include <Windows.h>
#include <mmsystem.h>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    // The engine drops held keys when it loses focus, so a held push-to-talk has to re-assert.
    constexpr double kReassertSeconds = 0.5;

    goldsrc::cvar_t* g_ptt_enabled = nullptr;
    goldsrc::cvar_t* g_ptt_key = nullptr;

    bool g_held = false;
    double g_last_assert = 0.0;

    // Start and stop blips, Discord-style. They cannot come from the engine: its mix is muted when
    // the window loses focus, which is exactly when the background push-to-talk runs.
    constexpr int kBeepRate = 48000;
    constexpr int kBeepMs = 70;
    constexpr double kBeepAmplitude = 0.22;

    std::vector<unsigned char> g_start_beep;
    std::vector<unsigned char> g_stop_beep;

    void AppendLe32(std::vector<unsigned char>& out, uint32_t value)
    {
        out.push_back(static_cast<unsigned char>(value & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    }

    void AppendLe16(std::vector<unsigned char>& out, uint16_t value)
    {
        out.push_back(static_cast<unsigned char>(value & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    }

    // A short, click-free sine blip as an in-memory WAV: PlaySound plays it on waveOut, a device the
    // game's own focus mute does not touch.
    std::vector<unsigned char> MakeBeepWav(int frequency)
    {
        const int sample_count = kBeepRate * kBeepMs / 1000;
        const int attack = kBeepRate / 200;   // 5 ms
        const int release = kBeepRate / 100;  // 10 ms

        std::vector<std::int16_t> samples(sample_count);

        for (int i = 0; i < sample_count; i++)
        {
            double envelope = 1.0;

            if (i < attack)
            {
                envelope = static_cast<double>(i) / attack;
            }
            else if (i >= sample_count - release)
            {
                envelope = static_cast<double>(sample_count - i) / release;
            }

            const double phase = 2.0 * 3.14159265358979323846 * frequency * i / kBeepRate;
            samples[i] = static_cast<std::int16_t>(std::sin(phase) * envelope * kBeepAmplitude * 32767.0);
        }

        const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(std::int16_t));

        std::vector<unsigned char> wav;
        wav.reserve(44 + data_size);

        const unsigned char riff[] = {'R', 'I', 'F', 'F'};
        const unsigned char wave[] = {'W', 'A', 'V', 'E'};
        const unsigned char fmt[] = {'f', 'm', 't', ' '};
        const unsigned char data[] = {'d', 'a', 't', 'a'};

        wav.insert(wav.end(), riff, riff + 4);
        AppendLe32(wav, 36 + data_size);
        wav.insert(wav.end(), wave, wave + 4);
        wav.insert(wav.end(), fmt, fmt + 4);
        AppendLe32(wav, 16);
        AppendLe16(wav, 1);              // PCM
        AppendLe16(wav, 1);              // mono
        AppendLe32(wav, kBeepRate);
        AppendLe32(wav, kBeepRate * 2);  // byte rate
        AppendLe16(wav, 2);              // block align
        AppendLe16(wav, 16);             // bits per sample
        wav.insert(wav.end(), data, data + 4);
        AppendLe32(wav, data_size);

        const unsigned char* raw = reinterpret_cast<const unsigned char*>(samples.data());
        wav.insert(wav.end(), raw, raw + data_size);

        return wav;
    }

    void PlayBeep(const std::vector<unsigned char>& wav)
    {
        if (!wav.empty())
        {
            // PlaySoundA, not the UNICODE macro: the image is bytes, not a wide string.
            PlaySoundA(reinterpret_cast<LPCSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
    }

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
        PlayBeep(g_stop_beep);
    }
} // namespace

void VoicePtt_Init()
{
    g_ptt_enabled = gEngfuncs.pfnRegisterVariable(cvars::kVoiceBackgroundPtt.name, cvars::kVoiceBackgroundPtt.value, goldsrc::FCVAR_ARCHIVE);
    g_ptt_key = gEngfuncs.pfnRegisterVariable(cvars::kVoiceBackgroundPttKey.name, cvars::kVoiceBackgroundPttKey.value, goldsrc::FCVAR_ARCHIVE);

    g_start_beep = MakeBeepWav(1046); // C6, the start blip
    g_stop_beep = MakeBeepWav(698);   // F5, the stop blip
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
        if (!g_held)
        {
            PlayBeep(g_start_beep);
        }

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
