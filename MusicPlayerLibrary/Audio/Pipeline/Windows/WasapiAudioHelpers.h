// SPDX-License-Identifier: MIT

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Audioclient.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "Audio/AudioOutputCapabilities.h"
#include "Audio/AudioOutputFormat.h"
#include "Core/NumericConversion.h"

namespace MusicPlayerLibrary
{
	inline constexpr REFERENCE_TIME WasapiReferenceTimeUnitsPerSecond = 10'000'000;

	[[nodiscard]] inline bool TryToFAudioWaveFormatExtensible(
		const WAVEFORMATEX* source,
		FAudioWaveFormatExtensible& result) noexcept
	{
		result = {};
		if (!source || source->nChannels == 0 || source->nSamplesPerSec == 0 ||
			source->nAvgBytesPerSec == 0 || source->nBlockAlign == 0 ||
			source->wBitsPerSample == 0 || source->wBitsPerSample % 8 != 0)
		{
			return false;
		}

		const auto bytes_per_sample = source->wBitsPerSample / 8;
		if (static_cast<std::uint32_t>(source->nChannels) * bytes_per_sample !=
			source->nBlockAlign ||
			static_cast<std::uint64_t>(source->nSamplesPerSec) *
				source->nBlockAlign != source->nAvgBytesPerSec)
		{
			return false;
		}

		result.Format.wFormatTag = source->wFormatTag;
		result.Format.nChannels = source->nChannels;
		result.Format.nSamplesPerSec = source->nSamplesPerSec;
		result.Format.nAvgBytesPerSec = source->nAvgBytesPerSec;
		result.Format.nBlockAlign = source->nBlockAlign;
		result.Format.wBitsPerSample = source->wBitsPerSample;

		if (source->wFormatTag == WAVE_FORMAT_PCM ||
			source->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		{
			result.Format.cbSize = 0;
			result.Samples.wValidBitsPerSample = source->wBitsPerSample;
			result.SubFormat = source->wFormatTag == WAVE_FORMAT_PCM
				? PcmAudioSubFormat
				: IeeeFloatAudioSubFormat;
			return true;
		}

		constexpr auto RequiredExtraBytes =
			sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		if (source->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
			source->cbSize < RequiredExtraBytes)
		{
			return false;
		}

		const auto* extensible =
			reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(source);
		const auto valid_bits = extensible->Samples.wValidBitsPerSample;
		if (valid_bits == 0 || valid_bits > source->wBitsPerSample ||
			(extensible->dwChannelMask != 0 &&
				std::popcount(extensible->dwChannelMask) != source->nChannels))
		{
			return false;
		}

		result.Format.cbSize = FAudioExtensibleFormatExtraSize;
		result.Samples.wValidBitsPerSample = valid_bits;
		result.dwChannelMask = extensible->dwChannelMask;
		result.SubFormat.Data1 = extensible->SubFormat.Data1;
		result.SubFormat.Data2 = extensible->SubFormat.Data2;
		result.SubFormat.Data3 = extensible->SubFormat.Data3;
		std::copy_n(extensible->SubFormat.Data4, 8, result.SubFormat.Data4);
		return GuidEquals(result.SubFormat, PcmAudioSubFormat) ||
			GuidEquals(result.SubFormat, IeeeFloatAudioSubFormat);
	}

	struct WasapiExclusiveProbeAxes final
	{
		std::vector<int> sample_rates;
		std::vector<AudioChannelMode> channel_modes;
		std::vector<AudioBitDepth> bit_depths;
	};

	[[nodiscard]] inline WasapiExclusiveProbeAxes
		BuildWasapiExclusiveProbeAxes(
			const AudioOutputFormat& requested,
			const AudioOutputFormat& preferred)
	{
		WasapiExclusiveProbeAxes result;
		const auto append_unique = []<typename Value>(
			std::vector<Value>& values,
			const Value value)
		{
			if (std::find(values.begin(), values.end(), value) == values.end())
				values.push_back(value);
		};

		if (requested.requested_sample_rate < 0)
			throw std::invalid_argument("Unsupported audio sample rate");
		if (requested.requested_sample_rate > 0)
			append_unique(result.sample_rates, requested.requested_sample_rate);
		if (preferred.sample_rate > 0)
			append_unique(result.sample_rates, preferred.sample_rate);
		append_unique(result.sample_rates, StandardAudioSampleRate);
		append_unique(result.sample_rates, 44'100);
		for (const int value : AudioOutputProbeSampleRates)
			append_unique(result.sample_rates, value);

		if (requested.requested_channel_mode == AudioChannelMode::Unknown)
			throw std::invalid_argument("Unsupported audio channel mode");
		if (requested.requested_channel_mode != AudioChannelMode::System)
		{
			append_unique(
				result.channel_modes, requested.requested_channel_mode);
		}
		const auto preferred_channel_mode = static_cast<AudioChannelMode>(
			GetAudioChannelTypeId(
				preferred.channel_count, preferred.channel_mask));
		if (preferred_channel_mode != AudioChannelMode::Unknown &&
			preferred_channel_mode != AudioChannelMode::System)
		{
			append_unique(result.channel_modes, preferred_channel_mode);
		}
		append_unique(result.channel_modes, AudioChannelMode::Stereo);
		append_unique(result.channel_modes, AudioChannelMode::Mono);
		for (const auto value : AudioOutputProbeChannelModes)
			append_unique(result.channel_modes, value);

		if (requested.requested_bit_depth == AudioBitDepth::Unknown)
			throw std::invalid_argument("Unsupported audio bit depth");
		if (requested.requested_bit_depth != AudioBitDepth::System)
			append_unique(result.bit_depths, requested.requested_bit_depth);
		if (preferred.bit_depth != AudioBitDepth::System &&
			preferred.bit_depth != AudioBitDepth::Unknown)
		{
			append_unique(result.bit_depths, preferred.bit_depth);
		}
		append_unique(result.bit_depths, AudioBitDepth::Bit24);
		append_unique(result.bit_depths, AudioBitDepth::Bit16);
		append_unique(result.bit_depths, AudioBitDepth::Bit32);
		return result;
	}

	[[nodiscard]] inline AudioOutputFormat MakeWasapiPcm32Variant(
		const AudioOutputFormat& source) noexcept
	{
		AudioOutputFormat result = source;
		result.bit_depth = AudioBitDepth::Bit32;
		result.sample_format = AV_SAMPLE_FMT_S32;
		result.wave_format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
		result.wave_format.Format.wBitsPerSample = 32;
		result.wave_format.Format.nBlockAlign = static_cast<std::uint16_t>(
			result.channel_count * sizeof(std::int32_t));
		result.wave_format.Format.nAvgBytesPerSec =
			result.wave_format.Format.nSamplesPerSec *
			result.wave_format.Format.nBlockAlign;
		result.wave_format.Format.cbSize = FAudioExtensibleFormatExtraSize;
		result.wave_format.Samples.wValidBitsPerSample = 32;
		result.wave_format.SubFormat = PcmAudioSubFormat;
		return result;
	}

	[[nodiscard]] inline bool TryMakeWasapiLegacyVariant(
		const AudioOutputFormat& source,
		AudioOutputFormat& result) noexcept
	{
		result = {};
		const auto& wave = source.wave_format;
		if (source.channel_count == 0 || source.channel_count > 2 ||
			wave.Format.wBitsPerSample == 0 ||
			wave.Samples.wValidBitsPerSample != wave.Format.wBitsPerSample)
		{
			return false;
		}

		std::uint16_t format_tag = 0;
		if (GuidEquals(wave.SubFormat, PcmAudioSubFormat))
			format_tag = FAUDIO_FORMAT_PCM;
		else if (GuidEquals(wave.SubFormat, IeeeFloatAudioSubFormat))
			format_tag = FAUDIO_FORMAT_IEEE_FLOAT;
		else
			return false;

		result = source;
		result.wave_format.Format.wFormatTag = format_tag;
		result.wave_format.Format.cbSize = 0;
		return true;
	}

	[[nodiscard]] inline bool AreWasapiWireFormatsEqual(
		const WAVEFORMATEXTENSIBLE& left,
		const WAVEFORMATEXTENSIBLE& right) noexcept
	{
		const auto& left_base = left.Format;
		const auto& right_base = right.Format;
		if (left_base.wFormatTag != right_base.wFormatTag ||
			left_base.nChannels != right_base.nChannels ||
			left_base.nSamplesPerSec != right_base.nSamplesPerSec ||
			left_base.nAvgBytesPerSec != right_base.nAvgBytesPerSec ||
			left_base.nBlockAlign != right_base.nBlockAlign ||
			left_base.wBitsPerSample != right_base.wBitsPerSample ||
			left_base.cbSize != right_base.cbSize)
		{
			return false;
		}
		if (left_base.wFormatTag != WAVE_FORMAT_EXTENSIBLE)
			return true;
		return left.Samples.wValidBitsPerSample ==
				right.Samples.wValidBitsPerSample &&
			left.dwChannelMask == right.dwChannelMask &&
			InlineIsEqualGUID(left.SubFormat, right.SubFormat) != FALSE;
	}

	class WasapiClockHealthMonitor final
	{
		struct Sample
		{
			std::uint64_t position = 0;
			std::uint64_t qpc_position_100ns = 0;
		};

		Sample window_start_{};
		std::uint64_t window_frequency_ = 0;
		std::uint32_t consecutive_slow_windows_ = 0;
		bool has_window_start_ = false;

		void StartWindow(
			const Sample sample,
			const std::uint64_t frequency,
			const bool clear_slow_history) noexcept
		{
			window_start_ = sample;
			window_frequency_ = frequency;
			has_window_start_ = true;
			if (clear_slow_history)
				consecutive_slow_windows_ = 0;
		}

	public:
		static constexpr std::uint64_t MinimumWindow100Nanoseconds = 2'500'000;
		static constexpr long double MinimumHealthyRateRatio = 0.65L;
		static constexpr std::uint32_t SlowWindowsBeforeRecovery = 2;

		void Reset() noexcept
		{
			window_start_ = {};
			window_frequency_ = 0;
			consecutive_slow_windows_ = 0;
			has_window_start_ = false;
		}

		[[nodiscard]] bool Observe(
			const std::uint64_t position,
			const std::uint64_t frequency,
			const std::uint64_t qpc_position_100ns) noexcept
		{
			if (frequency == 0)
			{
				Reset();
				return false;
			}

			const Sample sample{position, qpc_position_100ns};
			if (!has_window_start_ || frequency != window_frequency_ ||
				qpc_position_100ns < window_start_.qpc_position_100ns ||
				position < window_start_.position)
			{
				StartWindow(sample, frequency, true);
				return false;
			}

			const auto qpc_delta =
				qpc_position_100ns - window_start_.qpc_position_100ns;
			if (qpc_delta < MinimumWindow100Nanoseconds)
				return false;

			const auto position_delta = position - window_start_.position;
			const auto rate_ratio =
				static_cast<long double>(position_delta) *
				static_cast<long double>(WasapiReferenceTimeUnitsPerSecond) /
				(static_cast<long double>(qpc_delta) *
					static_cast<long double>(frequency));
			StartWindow(sample, frequency, false);

			if (rate_ratio >= MinimumHealthyRateRatio)
			{
				consecutive_slow_windows_ = 0;
				return false;
			}

			if (consecutive_slow_windows_ < SlowWindowsBeforeRecovery)
				++consecutive_slow_windows_;
			return consecutive_slow_windows_ >= SlowWindowsBeforeRecovery;
		}
	};

	[[nodiscard]] inline WAVEFORMATEXTENSIBLE ToWindowsWaveFormatExtensible(
		const AudioOutputFormat& format) noexcept
	{
		const auto& source = format.wave_format;
		WAVEFORMATEXTENSIBLE result{};
		result.Format.wFormatTag = source.Format.wFormatTag;
		result.Format.nChannels = source.Format.nChannels;
		result.Format.nSamplesPerSec = source.Format.nSamplesPerSec;
		result.Format.nAvgBytesPerSec = source.Format.nAvgBytesPerSec;
		result.Format.nBlockAlign = source.Format.nBlockAlign;
		result.Format.wBitsPerSample = source.Format.wBitsPerSample;
		result.Format.cbSize = source.Format.cbSize;
		result.Samples.wValidBitsPerSample = source.Samples.wValidBitsPerSample;
		result.dwChannelMask = source.dwChannelMask;
		result.SubFormat.Data1 = source.SubFormat.Data1;
		result.SubFormat.Data2 = source.SubFormat.Data2;
		result.SubFormat.Data3 = source.SubFormat.Data3;
		std::copy_n(source.SubFormat.Data4, 8, result.SubFormat.Data4);
		return result;
	}

	[[nodiscard]] inline std::uint32_t WasapiFramesFromReferenceTimeCeiling(
		const REFERENCE_TIME period,
		const int sample_rate) noexcept
	{
		if (period <= 0 || sample_rate <= 0)
			return 0;
		const auto frames =
			(static_cast<std::uint64_t>(period) *
				static_cast<std::uint64_t>(sample_rate) +
				WasapiReferenceTimeUnitsPerSecond - 1) /
			WasapiReferenceTimeUnitsPerSecond;
		return SaturateToUint32(frames);
	}
}

#endif
