// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

#include "Audio/AudioOutputFormat.h"

namespace MusicPlayerLibrary
{
	using AudioStreamGeneration = std::uint64_t;

	struct NormalizedPcmBlock
	{
		std::span<const std::uint8_t> bytes{};
		std::uint32_t frame_count = 0;
		double pts_seconds = 0.0;
		// Zero-based frame offset in this generation's normalized stream.
		std::uint64_t stream_frame_offset = 0;
		AudioStreamGeneration generation = 0;
		bool end_of_stream = false;
	};

	struct AudioSinkState
	{
		std::uint32_t buffers_queued = 0;
		// Source-voice frames consumed by the audio engine in this generation.
		std::uint64_t samples_played = 0;
		// Normalized media frames estimated to have reached the presentation
		// boundary after sink DSP and device latency. Unlike queued_frames this is
		// a playback clock and is suitable for timestamped observers.
		std::uint64_t media_frames_presented = 0;
		std::uint32_t presentation_latency_frames = 0;
		std::uint64_t queued_frames = 0;
		AudioStreamGeneration generation = 0;
		std::uint32_t error_code = 0;
		bool stream_ended = false;
	};

	class IAudioObserve
	{
	public:
		virtual ~IAudioObserve() = default;
		virtual void OnFormat(
			const AudioOutputFormat& format,
			AudioStreamGeneration generation) = 0;
		virtual void OnPcm(const NormalizedPcmBlock& block) = 0;
		virtual void OnReset(AudioStreamGeneration generation) = 0;
		virtual void OnEndOfStream(AudioStreamGeneration generation) = 0;
	};

	class IAudioSink
	{
	public:
		virtual ~IAudioSink() = default;
		[[nodiscard]] virtual const AudioOutputFormat& GetOutputFormat() const noexcept = 0;
		[[nodiscard]] virtual const AudioOutputFormat& GetDeviceFormat() const noexcept = 0;
		// Source/sink format handshake. A true result means the decoded PCM can
		// be submitted byte-for-byte without resampling, remixing, interleaving,
		// or sample-format conversion.
		[[nodiscard]] virtual bool HandshakeInputFormat(
			const DecodedAudioFormat& input) const noexcept
		{
			return AreAudioFormatsBitPerfect(input, GetOutputFormat());
		}
		[[nodiscard]] virtual bool IsInitialized() const noexcept = 0;
		[[nodiscard]] virtual bool IsLimiterEnabled() const noexcept
		{
			return false;
		}
		// Preferred source submission quantum. A value of zero leaves the
		// source's default block size unchanged.
		[[nodiscard]] virtual std::uint32_t GetPreferredSubmitFrameCount()
			const noexcept
		{
			return 0;
		}
		virtual AudioStreamGeneration BeginStream() = 0;
		virtual bool Submit(const NormalizedPcmBlock& block) = 0;
		virtual bool EndStream() noexcept = 0;
		virtual bool Start() noexcept = 0;
		virtual void Stop() noexcept = 0;
		virtual void AbortStream() noexcept = 0;
		[[nodiscard]] virtual AudioSinkState GetState() const noexcept = 0;
		virtual bool WaitForStreamEnd(std::chrono::milliseconds timeout) = 0;
		virtual void SetMasterVolume(float volume) noexcept = 0;
		[[nodiscard]] virtual int GetEqualizerBand(int index) const noexcept = 0;
		virtual void SetEqualizerBand(int index, int value) noexcept = 0;
		virtual bool IsExclusiveModeEnabled() noexcept = 0;
	};

	class IAudioSource
	{
	public:
		virtual ~IAudioSource() = default;
		virtual void Connect(std::shared_ptr<IAudioSink> sink) = 0;
		virtual void Subscribe(std::shared_ptr<IAudioObserve> observe) = 0;
		virtual void ClearObservers() = 0;
		[[nodiscard]] virtual const AudioOutputFormat& GetNormalizedFormat() const noexcept = 0;
		[[nodiscard]] virtual bool IsBitPerfect() const noexcept
		{
			return false;
		}
	};
}
