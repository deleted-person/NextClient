#include "voice.h"

#ifdef _WIN32

#include "voice_gain.h"
#include "engine.h"
#include "common/cvar.h"
#include "console/console.h"

#include <SDL.h>
#include <cvars/cvar_defaults.h>
#include <easylogging++.h>
#include <funchook.h>
#include <steam/steam_api.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    constexpr int kChannelCount = 4;
    constexpr int kDecodeSampleCapacity = kVoiceSampleRate * 4;
    constexpr double kIdleSeconds = 0.5;

    // The engine mixes voice into the same DirectSound stream as everything else, and that stream
    // is muted while the game is not the foreground window. Playing the decoded voice through a
    // separate SDL device keeps it audible when the game is minimized.
    struct PlaybackChannel
    {
        SDL_AudioDeviceID device{};
        int slot = -1;
        double last_activity{};
        VoiceGainState gain{};
        std::vector<std::int16_t> pending;
        std::size_t pending_offset{};
    };

    std::array<PlaybackChannel, kChannelCount> g_channels;
    std::vector<std::int16_t> g_decode_buffer;

    bool g_initialized = false;
    bool g_sdl_audio_initialized = false;
    bool g_status_registered = false;
    bool g_cvar_registered = false;
    int g_incoming_slot = -1;
    long long g_incoming_packets = 0;
    long long g_decoded_packets = 0;

    goldsrc::cvar_t* g_voice_scale = nullptr;
    goldsrc::cvar_t* g_voice_enable = nullptr;
    goldsrc::cvar_t* g_voice_background = nullptr;

    funchook_t* g_funchook = nullptr;
    void* g_trampoline = nullptr;

    // The engine's ISteamUser is called through thiscall; the hook takes the same ECX and stack
    // shape as fastcall with a dummy second argument.
    using DecompressVoice_t = EVoiceResult(__thiscall*)(ISteamUser*, const void*, std::uint32_t, void*, std::uint32_t, std::uint32_t*, std::uint32_t);
    DecompressVoice_t g_decompress_original = nullptr;

    double Now()
    {
        return realtime != nullptr ? *realtime : 0.0;
    }

    bool IsVoiceEnabled()
    {
        return g_voice_enable == nullptr || g_voice_enable->value != 0.0f;
    }

    // The private path is a NextClient feature; with it off the engine plays voice as it always did.
    bool IsBackgroundEnabled()
    {
        return g_voice_background == nullptr || g_voice_background->value != 0.0f;
    }

    // SDL 2.0.0 has no queue API, so the callback drains a buffer that the decode hook appends to.
    // The hook holds the device lock while appending, which keeps the callback off the buffer.
    void SDLCALL AudioCallback(void* userdata, Uint8* stream, int length)
    {
        PlaybackChannel& channel = *static_cast<PlaybackChannel*>(userdata);

        std::size_t out_count = static_cast<std::size_t>(length) / sizeof(std::int16_t);
        std::int16_t* out = reinterpret_cast<std::int16_t*>(stream);

        std::size_t available = channel.pending.size() - channel.pending_offset;
        std::size_t to_copy = std::min(available, out_count);

        if (to_copy > 0)
        {
            std::memcpy(out, channel.pending.data() + channel.pending_offset, to_copy * sizeof(std::int16_t));
            channel.pending_offset += to_copy;
        }

        if (to_copy < out_count)
        {
            std::memset(out + to_copy, 0, (out_count - to_copy) * sizeof(std::int16_t));
        }

        if (channel.pending_offset == channel.pending.size())
        {
            channel.pending.clear();
            channel.pending_offset = 0;
        }
    }

    void Enqueue(PlaybackChannel& channel, const std::int16_t* samples, std::size_t sample_count)
    {
        // Cap the backlog so a stalled consumer cannot grow it without bound.
        constexpr std::size_t kMaxPending = static_cast<std::size_t>(kVoiceSampleRate) * 2;

        SDL_LockAudioDevice(channel.device);

        if (channel.pending_offset > 0)
        {
            channel.pending.erase(channel.pending.begin(), channel.pending.begin() + static_cast<std::ptrdiff_t>(channel.pending_offset));
            channel.pending_offset = 0;
        }

        channel.pending.insert(channel.pending.end(), samples, samples + sample_count);

        if (channel.pending.size() > kMaxPending)
        {
            channel.pending.erase(channel.pending.begin(), channel.pending.begin() + static_cast<std::ptrdiff_t>(channel.pending.size() - kMaxPending));
        }

        SDL_UnlockAudioDevice(channel.device);
    }

    void CloseDevices()
    {
        for (PlaybackChannel& channel : g_channels)
        {
            if (channel.device != 0)
            {
                SDL_CloseAudioDevice(channel.device);
            }

            channel = PlaybackChannel{};
        }
    }

    // Returns the channel that owns the slot, reusing an idle one first and otherwise the one that
    // has been quiet longest, so an inactive speaker cannot hold a device against a live one.
    PlaybackChannel& AcquireChannel(int slot)
    {
        double now = Now();
        PlaybackChannel* oldest = &g_channels[0];

        for (PlaybackChannel& channel : g_channels)
        {
            if (channel.slot == slot)
            {
                return channel;
            }

            if (channel.slot == -1 || now - channel.last_activity > kIdleSeconds)
            {
                channel.slot = slot;
                channel.gain = VoiceGainState{};
                return channel;
            }

            if (channel.last_activity < oldest->last_activity)
            {
                oldest = &channel;
            }
        }

        oldest->slot = slot;
        oldest->gain = VoiceGainState{};
        return *oldest;
    }

    void VoiceStatus_f()
    {
        Con_Printf("voice path: %s, sdl audio: %s\n", g_initialized ? "private" : "engine", g_sdl_audio_initialized ? "on" : "off");

        if (g_voice_scale != nullptr)
        {
            Con_Printf("  voice_scale %.3f\n", g_voice_scale->value);
        }
        else
        {
            Con_Printf("  voice_scale missing\n");
        }

        Con_Printf("  voice_enable %s\n",
            g_voice_enable == nullptr ? "missing" : (g_voice_enable->value != 0.0f ? "on" : "off"));
        Con_Printf("  voice_background %s\n",
            g_voice_background == nullptr ? "missing" : (g_voice_background->value != 0.0f ? "on" : "off"));
        Con_Printf("  incoming %lld packets, decoded %lld, current slot %d\n", g_incoming_packets, g_decoded_packets, g_incoming_slot);

        for (int i = 0; i < kChannelCount; ++i)
        {
            Con_Printf("  channel %d: slot %d\n", i, g_channels[i].slot);
        }
    }

    EVoiceResult __fastcall DecompressVoice_Hook(
        ISteamUser* self,
        int,
        const void* compressed,
        std::uint32_t cb_compressed,
        void* dest,
        std::uint32_t dest_size,
        std::uint32_t* bytes_written,
        std::uint32_t desired_sample_rate
    )
    {
        if (g_decompress_original == nullptr)
        {
            return k_EVoiceResultNotInitialized;
        }

        if (g_incoming_slot < 0 || dest == nullptr || bytes_written == nullptr)
        {
            return g_decompress_original(self, compressed, cb_compressed, dest, dest_size, bytes_written, desired_sample_rate);
        }

        std::uint32_t written = 0;
        EVoiceResult result = g_decompress_original(
            self,
            compressed,
            cb_compressed,
            g_decode_buffer.data(),
            static_cast<std::uint32_t>(g_decode_buffer.size() * sizeof(std::int16_t)),
            &written,
            kVoiceSampleRate
        );

        if (result != k_EVoiceResultOK)
        {
            // The private decode failed; let the engine attempt its own.
            return g_decompress_original(self, compressed, cb_compressed, dest, dest_size, bytes_written, desired_sample_rate);
        }

        if (written > 0)
        {
            int sample_count = static_cast<int>(written / sizeof(std::int16_t));

            PlaybackChannel& channel = AcquireChannel(g_incoming_slot);
            float voice_scale = g_voice_scale != nullptr ? g_voice_scale->value : 1.0f;

            VoiceGain_Normalize(g_decode_buffer.data(), sample_count, voice_scale, channel.gain);
            channel.last_activity = Now();

            Enqueue(channel, g_decode_buffer.data(), static_cast<std::size_t>(sample_count));

            ++g_decoded_packets;
        }

        // The private path already plays the voice, so the engine must not play it a second time.
        std::memset(dest, 0, dest_size);
        *bytes_written = dest_size;

        return k_EVoiceResultOK;
    }
} // namespace

void Voice_Init()
{
    if (!g_status_registered)
    {
        // The engine's Cmd_AddCommand refuses calls after host_initialized; the client-facing
        // command table is the one that still accepts registrations here.
        gEngfuncs.pfnAddCommand("voice_status", VoiceStatus_f);
        g_status_registered = true;
    }

    if (!g_cvar_registered)
    {
        g_voice_background = gEngfuncs.pfnRegisterVariable(cvars::kVoiceBackground.name, cvars::kVoiceBackground.value, goldsrc::FCVAR_ARCHIVE);
        g_cvar_registered = true;
    }

    if (g_initialized)
    {
        return;
    }

    g_voice_scale = Cvar_FindVar("voice_scale");
    g_voice_enable = Cvar_FindVar("voice_enable");

    if (g_voice_scale == nullptr)
    {
        LOG(WARNING) << "[voice] voice_scale was not found, voice stays with the engine";
        return;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        LOG(WARNING) << "[voice] SDL audio was not initialized: " << SDL_GetError();
        return;
    }

    g_sdl_audio_initialized = true;

    bool devices_open = true;

    for (PlaybackChannel& channel : g_channels)
    {
        SDL_AudioSpec spec{};
        spec.freq = kVoiceSampleRate;
        spec.format = AUDIO_S16SYS;
        spec.channels = 1;
        spec.samples = 1024;
        spec.callback = AudioCallback;
        spec.userdata = &channel;

        channel.device = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);

        if (channel.device == 0)
        {
            devices_open = false;
            break;
        }

        SDL_PauseAudioDevice(channel.device, 0);
    }

    if (!devices_open)
    {
        LOG(WARNING) << "[voice] SDL audio device was not opened: " << SDL_GetError();
        Voice_Shutdown();
        return;
    }

    g_decode_buffer.resize(kDecodeSampleCapacity);

    ISteamUser* user = SteamUser();

    if (user == nullptr)
    {
        LOG(WARNING) << "[voice] SteamUser is unavailable, voice stays with the engine";
        Voice_Shutdown();
        return;
    }

    void** vtable = *reinterpret_cast<void***>(user);
    g_trampoline = vtable[11];

    g_funchook = funchook_create();

    if (g_funchook == nullptr ||
        funchook_prepare(g_funchook, &g_trampoline, reinterpret_cast<void*>(&DecompressVoice_Hook)) != FUNCHOOK_ERROR_SUCCESS)
    {
        LOG(WARNING) << "[voice] DecompressVoice could not be intercepted, voice stays with the engine";
        Voice_Shutdown();
        return;
    }

    g_decompress_original = reinterpret_cast<DecompressVoice_t>(g_trampoline);

    if (funchook_install(g_funchook, 0) != FUNCHOOK_ERROR_SUCCESS)
    {
        LOG(WARNING) << "[voice] DecompressVoice hook could not be installed, voice stays with the engine";
        Voice_Shutdown();
        return;
    }

    g_initialized = true;
}

void Voice_Shutdown()
{
    if (g_funchook != nullptr)
    {
        funchook_uninstall(g_funchook, 0);
        funchook_destroy(g_funchook);
        g_funchook = nullptr;
    }

    g_decompress_original = nullptr;
    g_trampoline = nullptr;

    CloseDevices();

    if (g_sdl_audio_initialized)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_sdl_audio_initialized = false;
    }

    g_decode_buffer.clear();
    g_initialized = false;
    g_incoming_slot = -1;
}

void Voice_BeginIncoming(int slot)
{
    bool armed = g_initialized && IsVoiceEnabled() && IsBackgroundEnabled() && slot >= 0;

    if (armed)
    {
        ++g_incoming_packets;
    }

    g_incoming_slot = armed ? slot : -1;
}

#else

void Voice_Init()
{
}

void Voice_Shutdown()
{
}

void Voice_BeginIncoming(int)
{
}

#endif
