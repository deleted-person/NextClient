#pragma once

#include <cstdint>

// Incoming voice is decoded at this rate; the regain ramp is expressed in samples at it.
constexpr int kVoiceSampleRate = 48000;

// The scale is carried between buffers so the gain ramps instead of jumping: the loudness of
// each buffer is re-estimated and an abrupt rise between words must not become an abrupt boost.
struct VoiceGainState
{
    float previous_scale{};
};

// Scales 16-bit mono PCM in place so its peak approaches the ideal gain scaled by voice_scale.
// A silent buffer leaves the state untouched.
void VoiceGain_Normalize(std::int16_t* samples, int sample_count, float voice_scale, VoiceGainState& state);
