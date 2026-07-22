// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/DSP/PcmSampleConversion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
	[[nodiscard]] bool HasSampleCapacity(
		const std::size_t byte_count,
		const std::size_t sample_count,
		const std::size_t bytes_per_sample) noexcept
	{
		return sample_count <=
			(std::numeric_limits<std::size_t>::max)() / bytes_per_sample &&
			byte_count >= sample_count * bytes_per_sample;
	}

	[[nodiscard]] float SanitizePcmFloat(const float input) noexcept
	{
		return std::isfinite(input)
			? std::clamp(input, -1.0f, 1.0f)
			: 0.0f;
	}
}

bool MusicPlayerLibrary::AudioDsp::DecodePcmSamples(
	const std::span<const std::uint8_t> source,
	const std::span<float> destination,
	const AudioBitDepth bit_depth,
	const AVSampleFormat sample_format) noexcept
{
	switch (sample_format)
	{
	case AV_SAMPLE_FMT_S16:
		if (bit_depth != AudioBitDepth::Bit16)
			return false;
		if (!HasSampleCapacity(source.size(), destination.size(), sizeof(std::int16_t)))
			return false;
		for (std::size_t index = 0; index < destination.size(); ++index)
		{
			std::int16_t value = 0;
			std::memcpy(
				&value, source.data() + index * sizeof(value), sizeof(value));
			destination[index] = static_cast<float>(value) / 32768.0f;
		}
		return true;

	case AV_SAMPLE_FMT_S32:
		if (bit_depth != AudioBitDepth::Bit24 &&
			bit_depth != AudioBitDepth::Bit32)
		{
			return false;
		}
		if (!HasSampleCapacity(source.size(), destination.size(), sizeof(std::int32_t)))
			return false;
		for (std::size_t index = 0; index < destination.size(); ++index)
		{
			// PCM24 is left-aligned in the same signed S32 container used by
			// full-range PCM32, so both normalize against 2^31.
			std::int32_t value = 0;
			std::memcpy(
				&value, source.data() + index * sizeof(value), sizeof(value));
			destination[index] = static_cast<float>(
				static_cast<double>(value) / 2147483648.0);
		}
		return true;

	case AV_SAMPLE_FMT_FLT:
		if (bit_depth != AudioBitDepth::Bit32)
			return false;
		if (!HasSampleCapacity(source.size(), destination.size(), sizeof(float)))
			return false;
		for (std::size_t index = 0; index < destination.size(); ++index)
		{
			float value = 0.0f;
			std::memcpy(
				&value, source.data() + index * sizeof(value), sizeof(value));
			destination[index] = std::isfinite(value) ? value : 0.0f;
		}
		return true;

	default:
		return false;
	}
}

bool MusicPlayerLibrary::AudioDsp::EncodePcmSamples(
	const std::span<const float> source,
	const std::span<std::uint8_t> destination,
	const AudioBitDepth bit_depth,
	const AVSampleFormat sample_format) noexcept
{
	switch (sample_format)
	{
	case AV_SAMPLE_FMT_S16:
		if (bit_depth != AudioBitDepth::Bit16)
			return false;
		if (!HasSampleCapacity(destination.size(), source.size(), sizeof(std::int16_t)))
			return false;
		for (std::size_t index = 0; index < source.size(); ++index)
		{
			const float sample = SanitizePcmFloat(source[index]);
			const auto scaled = sample <= -1.0f
				? static_cast<long>(-32768)
				: sample >= 1.0f
					? static_cast<long>(32767)
					: std::lround(static_cast<double>(sample) * 32768.0);
			const auto value = static_cast<std::int16_t>(
				std::clamp(scaled, static_cast<long>(-32768),
					static_cast<long>(32767)));
			std::memcpy(
				destination.data() + index * sizeof(value), &value, sizeof(value));
		}
		return true;

	case AV_SAMPLE_FMT_S32:
		if (bit_depth != AudioBitDepth::Bit24 &&
			bit_depth != AudioBitDepth::Bit32)
		{
			return false;
		}
		if (!HasSampleCapacity(destination.size(), source.size(), sizeof(std::int32_t)))
			return false;
		for (std::size_t index = 0; index < source.size(); ++index)
		{
			const float sample = SanitizePcmFloat(source[index]);
			std::int32_t value = 0;
			if (bit_depth == AudioBitDepth::Bit24)
			{
				const auto scaled = sample <= -1.0f
					? static_cast<std::int64_t>(-8'388'608)
					: sample >= 1.0f
						? static_cast<std::int64_t>(8'388'607)
						: static_cast<std::int64_t>(std::llround(
							static_cast<double>(sample) * 8'388'608.0));
				const auto valid = std::clamp(
					scaled,
					static_cast<std::int64_t>(-8'388'608),
					static_cast<std::int64_t>(8'388'607));
				value = static_cast<std::int32_t>(valid * 256);
			}
			else
			{
				const auto scaled = sample <= -1.0f
					? static_cast<std::int64_t>(-2'147'483'648LL)
					: sample >= 1.0f
						? static_cast<std::int64_t>(2'147'483'647LL)
						: static_cast<std::int64_t>(std::llround(
							static_cast<double>(sample) * 2'147'483'648.0));
				value = static_cast<std::int32_t>((std::clamp)(
					scaled,
					static_cast<std::int64_t>(-2'147'483'648LL),
					static_cast<std::int64_t>(2'147'483'647LL)));
			}
			std::memcpy(
				destination.data() + index * sizeof(value), &value, sizeof(value));
		}
		return true;

	case AV_SAMPLE_FMT_FLT:
		if (bit_depth != AudioBitDepth::Bit32)
			return false;
		if (!HasSampleCapacity(destination.size(), source.size(), sizeof(float)))
			return false;
		for (std::size_t index = 0; index < source.size(); ++index)
		{
			const float sample = SanitizePcmFloat(source[index]);
			std::memcpy(
				destination.data() + index * sizeof(sample),
				&sample,
				sizeof(sample));
		}
		return true;

	default:
		return false;
	}
}
