// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <FAudio.h>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/samplefmt.h>
#if defined(__cplusplus)
}
#endif

namespace MusicPlayerLibrary
{
	inline constexpr int StandardAudioSampleRate = 48'000;
	inline constexpr std::uint16_t FAudioExtensibleFormatExtraSize =
		static_cast<std::uint16_t>(
			sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx));
	inline constexpr FAudioGUID PcmAudioSubFormat{
		0x00000001, 0x0000, 0x0010,
		{0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
	};
	inline constexpr FAudioGUID IeeeFloatAudioSubFormat{
		0x00000003, 0x0000, 0x0010,
		{0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
	};

	enum class AudioBackend : int
	{
		FAudio = 0,
#if defined(_WIN32)
		WasapiExclusive = 1
#endif
	};

	struct AudioOutputDeviceInfo
	{
		// Stable backend-specific identifier encoded as UTF-8. An empty requested
		// identifier selects the backend's current default rendering device.
		std::string id;
		std::string display_name;
		bool is_default = false;
	};

	enum class AudioChannelMode : int
	{
		Unknown = -1,
		System = 0,
		Mono = 1,
		Stereo = 2,
		Surround51 = 3,
		Surround71 = 4
	};

	enum class AudioBitDepth : int
	{
		Unknown = -1,
		System = 0,
		Bit16 = 16,
		Bit24 = 24,
		Bit32 = 32
	};

	// Plain format metadata returned across the native API boundary. IDs use
	// AudioChannelMode/AudioBitDepth values; -1 means unknown and 0 means a
	// system-configured request.
	struct AudioFormatInfo
	{
		int channel_type_id = static_cast<int>(AudioChannelMode::Unknown);
		int sample_rate = 0;
		int bit_depth = static_cast<int>(AudioBitDepth::Unknown);
	};

	// The decoded PCM contract offered by an audio source to a sink.  Unlike
	// AudioFormatInfo this includes the physical FFmpeg sample representation,
	// so planar/packed or integer/float conversions cannot be mistaken for a
	// bit-perfect route.
	struct DecodedAudioFormat
	{
		int sample_rate = 0;
		std::uint16_t channel_count = 0;
		std::uint64_t channel_mask = 0;
		int bit_depth = static_cast<int>(AudioBitDepth::Unknown);
		AVSampleFormat sample_format = AV_SAMPLE_FMT_NONE;
	};

	class AudioBitrateTracker
	{
		std::uint64_t encoded_bytes_ = 0;
		double decoded_duration_seconds_ = 0.0;
		std::atomic<double> kbytes_per_second_{ 0.0 };

	public:
		void Reset() noexcept;
		void ObserveEncodedBytes(std::uint64_t encoded_bytes) noexcept;
		void ObserveDecodedSamples(int sample_count, int sample_rate) noexcept;
		[[nodiscard]] double GetKBytesPerSecond() const noexcept;
	};

	struct AudioOutputFormat
	{
		AudioBackend requested_backend = AudioBackend::FAudio;
		std::string requested_device_id;

		// A zero sample rate and System enum values request the current default
		// rendering-device format.
		int requested_sample_rate = 0;
		AudioChannelMode requested_channel_mode = AudioChannelMode::System;
		AudioBitDepth requested_bit_depth = AudioBitDepth::System;

		int sample_rate = 0;
		std::uint16_t channel_count = 0;
		std::uint32_t channel_mask = 0;
		AudioBitDepth bit_depth = AudioBitDepth::System;
		AVSampleFormat sample_format = AV_SAMPLE_FMT_NONE;
		std::string ffmpeg_channel_layout;
		FAudioWaveFormatExtensible wave_format{};
	};

	[[nodiscard]] bool GuidEquals(
		const FAudioGUID& left,
		const FAudioGUID& right) noexcept;

	// Compares the resolved PCM representation consumed by DSP/observer code.
	// Backend choice and the original request are intentionally not part of the
	// comparison once both formats have been resolved.
	[[nodiscard]] bool AreResolvedPcmFormatsEqual(
		const AudioOutputFormat& left,
		const AudioOutputFormat& right) noexcept;

	[[nodiscard]] FAudioWaveFormatExtensible
		MakeFallbackAudioWaveFormat() noexcept;

	// 根据显式指定的设备格式进行解析。将其设为公共方法，以便无需打开音频设备即可测试格式映射。
	AudioOutputFormat ResolveAudioOutputFormat(
		const AudioOutputFormat& requested,
		const FAudioWaveFormatExtensible& system_format);

	// 根据请求的配置，解析当前设备能提供的输出格式
	AudioOutputFormat ResolveAudioOutputFormat(
		const AudioOutputFormat& requested = {});

	[[nodiscard]] int GetAudioChannelTypeId(
		int channel_count,
		std::uint64_t channel_mask = 0) noexcept;
	[[nodiscard]] AudioFormatInfo GetAudioFormatInfo(
		const AudioOutputFormat& format) noexcept;
	[[nodiscard]] bool AreAudioFormatsBitPerfect(
		const DecodedAudioFormat& input,
		const AudioOutputFormat& output) noexcept;

	// Calculates the observed encoded-audio byte rate using decimal KBytes
	// (1 KByte = 1000 bytes).
	double CalculateAudioBitrateKBytesPerSecond(
		std::uint64_t encoded_bytes,
		double decoded_duration_seconds) noexcept;

	// Estimates the whole-stream average bitrate from its backing byte length.
	double CalculateAverageAudioBitrateBitsPerSecond(
		std::uint64_t stream_length_bytes,
		double duration_seconds) noexcept;

	[[nodiscard]] bool IsLoselessAudio(double average_bitrate_bits_per_second) noexcept;
	[[nodiscard]] bool IsHiResAudio(int source_sample_rate) noexcept;
}
