// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <span>

#include "Audio/AudioOutputFormat.h"

namespace MusicPlayerLibrary::AudioDsp
{
	// Convert an entire interleaved PCM buffer while selecting its physical
	// representation only once per call.
	[[nodiscard]] bool DecodePcmSamples(
		std::span<const std::uint8_t> source,
		std::span<float> destination,
		AudioBitDepth bit_depth,
		AVSampleFormat sample_format) noexcept;

	[[nodiscard]] bool EncodePcmSamples(
		std::span<const float> source,
		std::span<std::uint8_t> destination,
		AudioBitDepth bit_depth,
		AVSampleFormat sample_format) noexcept;
}
