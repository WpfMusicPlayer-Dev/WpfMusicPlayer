// SPDX-License-Identifier: MIT

#pragma once

#if defined(_WIN32)

#include <chrono>
#include <memory>

#include "Audio/Pipeline/AudioPipeline.h"

namespace MusicPlayerLibrary
{
	class WasapiExclusiveOutputDevice;

	/**
	 * @brief Event-driven WASAPI exclusive-mode sink.
	 *
	 * The implementation owns one IAudioClient/IAudioRenderClient pair. PCM is
	 * copied into a generation-scoped queue and consumed by the WASAPI render
	 * event thread. A flat equalizer takes the byte-preserving fast path; active
	 * bands are processed by AudioDsp::EqualizerDsp before the render buffer is
	 * released to the endpoint.
	 */
	class WasapiExclusiveSink final : public IAudioSink
	{
		struct Impl;
		std::unique_ptr<Impl> impl_;

	public:
		explicit WasapiExclusiveSink(
			const AudioOutputFormat& requested = {},
			std::shared_ptr<WasapiExclusiveOutputDevice> device = nullptr);
		~WasapiExclusiveSink() override;

		WasapiExclusiveSink(const WasapiExclusiveSink&) = delete;
		WasapiExclusiveSink& operator=(const WasapiExclusiveSink&) = delete;

		[[nodiscard]] const AudioOutputFormat& GetOutputFormat() const noexcept override;
		[[nodiscard]] const AudioOutputFormat& GetDeviceFormat() const noexcept override;
		[[nodiscard]] bool IsInitialized() const noexcept override;
		[[nodiscard]] bool IsLimiterEnabled() const noexcept override;
		[[nodiscard]] std::uint32_t GetPreferredSubmitFrameCount()
			const noexcept override;
		AudioStreamGeneration BeginStream() override;
		bool Submit(const NormalizedPcmBlock& block) override;
		bool EndStream() noexcept override;
		bool Start() noexcept override;
		void Stop() noexcept override;
		void AbortStream() noexcept override;
		[[nodiscard]] AudioSinkState GetState() const noexcept override;
		bool WaitForStreamEnd(std::chrono::milliseconds timeout) override;
		void SetMasterVolume(float volume) noexcept override;
		[[nodiscard]] int GetEqualizerBand(int index) const noexcept override;
		void SetEqualizerBand(int index, int value) noexcept override;

		[[nodiscard]] bool IsExclusiveModeEnabled() noexcept override
		{
			return true;
		}

		[[nodiscard]] std::shared_ptr<WasapiExclusiveOutputDevice>
			GetDevice() const noexcept;
	};
}

#endif // defined(_WIN32)
