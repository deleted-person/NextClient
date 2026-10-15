#include "client/voice_gain.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace
{
    constexpr int kSampleCount = 512;

    std::array<std::int16_t, kSampleCount> MakeBuffer(std::int16_t value)
    {
        std::array<std::int16_t, kSampleCount> samples{};
        samples.fill(value);
        return samples;
    }
} // namespace

TEST(VoiceGain, SilenceLeavesBufferAndStateUntouched)
{
    std::array<std::int16_t, kSampleCount> samples{};
    VoiceGainState state{};
    state.previous_scale = 2.0f;

    VoiceGain_Normalize(samples.data(), static_cast<int>(samples.size()), 1.0f, state);

    for (std::int16_t sample : samples)
    {
        EXPECT_EQ(sample, 0);
    }

    EXPECT_FLOAT_EQ(state.previous_scale, 2.0f);
}

TEST(VoiceGain, WeakSignalIsAmplified)
{
    auto samples = MakeBuffer(1000);

    VoiceGainState state{};
    state.previous_scale = 1.0f;

    VoiceGain_Normalize(samples.data(), static_cast<int>(samples.size()), 1.0f, state);

    EXPECT_GT(samples.back(), 1000);
    EXPECT_GT(state.previous_scale, 1.0f);
}

TEST(VoiceGain, NearSilenceKeepsTheCurrentScale)
{
    auto samples = MakeBuffer(1);

    VoiceGainState state{};

    VoiceGain_Normalize(samples.data(), static_cast<int>(samples.size()), 1.0f, state);

    for (std::int16_t sample : samples)
    {
        EXPECT_EQ(sample, 0);
    }

    EXPECT_FLOAT_EQ(state.previous_scale, 0.0f);
}

TEST(VoiceGain, ScaleRampsAcrossBuffers)
{
    VoiceGainState state{};
    state.previous_scale = 1.0f;

    auto first_buffer = MakeBuffer(1000);
    VoiceGain_Normalize(first_buffer.data(), static_cast<int>(first_buffer.size()), 1.0f, state);
    float first_scale = state.previous_scale;

    auto second_buffer = MakeBuffer(1000);
    VoiceGain_Normalize(second_buffer.data(), static_cast<int>(second_buffer.size()), 1.0f, state);
    float second_scale = state.previous_scale;

    EXPECT_GT(first_scale, 1.0f);
    EXPECT_GT(second_scale, first_scale);
    EXPECT_LT(second_scale, 4.0f);
}

TEST(VoiceGain, OutputIsClampedToSampleRange)
{
    auto samples = MakeBuffer(32000);

    VoiceGainState state{};
    state.previous_scale = 12.0f;

    VoiceGain_Normalize(samples.data(), static_cast<int>(samples.size()), 1.0f, state);

    // A high scale on a loud buffer would overflow a 16-bit sample and invert its sign.
    EXPECT_EQ(samples.front(), 32767);
    EXPECT_LT(state.previous_scale, 12.0f);
}
