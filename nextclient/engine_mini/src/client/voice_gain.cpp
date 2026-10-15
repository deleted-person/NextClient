#include "voice_gain.h"

#include <cstdlib>

namespace
{
    constexpr float kIdealGain = 4000.0f; // 10 ^ (72 dB / 20)
    constexpr float kMaxScale = 12.0f;    // ignore a sample that would be scaled past this
    constexpr float kMaxSample = 32767.0f;
    constexpr float kMinSample = -32768.0f;
} // namespace

void VoiceGain_Normalize(std::int16_t* samples, int sample_count, float voice_scale, VoiceGainState& state)
{
    int max_gain = 0;

    for (int i = 0; i < sample_count; ++i)
    {
        int magnitude = std::abs(static_cast<int>(samples[i]));

        if (magnitude > max_gain)
        {
            max_gain = magnitude;
        }
    }

    if (max_gain == 0)
    {
        return;
    }

    float target_scale = (voice_scale * kIdealGain) / static_cast<float>(max_gain);
    float scale = state.previous_scale;

    // The signal drops to near silence between words, which would otherwise ask for a huge boost.
    if (target_scale > kMaxScale)
    {
        target_scale = scale;
    }

    float increment = (target_scale - scale) / static_cast<float>(sample_count);

    // Regain volume over two seconds instead of within one buffer.
    if (target_scale >= 1.0f)
    {
        increment *= static_cast<float>(sample_count) / (static_cast<float>(kVoiceSampleRate) * 2.0f);
    }

    for (int i = 0; i < sample_count; ++i)
    {
        float value = static_cast<float>(samples[i]) * scale;
        scale += increment;

        if (value > kMaxSample)
        {
            value = kMaxSample;
        }
        else if (value < kMinSample)
        {
            value = kMinSample;
        }

        samples[i] = static_cast<std::int16_t>(value);
    }

    state.previous_scale = scale;
}
