// SPDX-License-Identifier: MIT

#pragma once

#include <array>

#include "Audio/AudioOutputFormat.h"

namespace MusicPlayerLibrary
{
	// Ordered by backend preference. These are the only formats probed when a
	// backend must discover a concrete output format without a device mix format.
	inline constexpr std::array<int, 9> AudioOutputProbeSampleRates{
		192000, 96000, 88200, StandardAudioSampleRate,
		44100, 22050, 16000, 11025, 8000
	};

	inline constexpr std::array<AudioChannelMode, 4> AudioOutputProbeChannelModes{
		AudioChannelMode::Surround71,
		AudioChannelMode::Surround51,
		AudioChannelMode::Stereo,
		AudioChannelMode::Mono
	};

	inline constexpr std::array<AudioBitDepth, 3> AudioOutputProbeBitDepths{
		AudioBitDepth::Bit32,
		AudioBitDepth::Bit24,
		AudioBitDepth::Bit16
	};
}
