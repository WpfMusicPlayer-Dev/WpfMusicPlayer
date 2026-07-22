// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <FAudio.h>
#include "Audio/FAudioResource.h"
#include "Audio/Pipeline/AudioPipeline.h"
#include "Audio/DSP/EqualizerSettings.h"
#include "Audio/Pipeline/Device/Common/FAudioOutputDevice.h"
#include "Audio/Pipeline/Sink/Common/FapoEqualizer.h"

namespace MusicPlayerLibrary
{
	/**
	 * @brief FAudio sink, default audio sink implementation.
	 * Cross-platform, shared-mode only.
	 */
	class FAudioSink final : public IAudioSink
	{
		static constexpr std::size_t BufferPoolSize = 64;
		static constexpr std::size_t PlaybackClockCapacity = 512;

		enum class StreamPhase : std::uint8_t
		{
			idle,
			open,
			draining,
			eos_queued,
			engine_ended,
			ended,
			aborted,
			error
		};

		struct BufferSlot
		{
			FAudioBuffer descriptor{};
			std::vector<std::uint8_t> storage;
			std::uint32_t frame_count = 0;
			AudioStreamGeneration generation = 0;
			bool end_of_stream = false;
			bool tail_probe = false;
			bool in_use = false;
			bool frames_accounted = false;
			std::uint64_t tail_probe_serial = 0;
			std::uint64_t tail_probe_process_sequence = 0;
		};

		struct VoiceCallbackContext
		{
			FAudioVoiceCallback callbacks{};
			FAudioSink* owner = nullptr;
		};

		std::shared_ptr<FAudioOutputDevice> device_;
		AudioOutputFormat output_format_{};
		UniqueFAudioVoice source_voice_;
		AudioDsp::UniqueFapo equalizer_effect_;
		VoiceCallbackContext voice_callback_{};

		mutable std::mutex submit_mutex_;
		mutable std::mutex buffer_mutex_;
		std::condition_variable buffer_cv_;
		std::array<std::unique_ptr<BufferSlot>, BufferPoolSize> buffer_slots_{};
		std::array<BufferSlot*, BufferPoolSize> free_buffers_{};
		std::size_t free_buffer_count_ = 0;
		std::size_t in_flight_count_ = 0;
		BufferSlot* pending_eos_callback_ = nullptr;
		std::uint64_t queued_frames_ = 0;
		std::uint64_t next_tail_probe_serial_ = 0;
		std::uint64_t completed_tail_probe_serial_ = 0;
		AudioStreamGeneration completed_tail_probe_generation_ = 0;
		std::uint64_t completed_tail_probe_process_sequence_ = 0;

		mutable std::mutex effect_mutex_;
		AudioDsp::EqualizerSettings equalizer_settings_;

		std::atomic<AudioStreamGeneration> generation_{0};
		std::atomic<AudioStreamGeneration> engine_completed_generation_{0};
		mutable std::atomic<AudioStreamGeneration> completed_generation_{0};
		std::atomic<std::int64_t> engine_end_clock_nanoseconds_{0};
		std::atomic<std::uint64_t> session_start_samples_{0};
		std::atomic<std::uint64_t> submitted_media_frames_{0};
		std::uint32_t limiter_latency_frames_ = 0;
		std::atomic<std::uint32_t> effect_latency_frames_{0};
		std::atomic_bool is_limiter_enabled_{false};
		struct PlaybackClockPoint
		{
			std::atomic<std::uint64_t> sequence{0};
			std::atomic<AudioStreamGeneration> generation{0};
			std::atomic<std::int64_t> captured_at_nanoseconds{0};
			std::atomic<std::uint64_t> mixed_media_frames{0};
		};
		std::array<PlaybackClockPoint, PlaybackClockCapacity> playback_clock_{};
		std::atomic<std::uint64_t> playback_clock_write_sequence_{0};
		mutable std::atomic<std::uint64_t> last_samples_played_{0};
		mutable std::atomic<std::uint64_t> last_media_frames_presented_{0};
		std::atomic<std::uint32_t> voice_error_{0};
		mutable std::atomic<StreamPhase> stream_phase_{StreamPhase::idle};
		std::atomic_bool aborting_{false};
		mutable std::mutex stream_end_mutex_;
		mutable std::condition_variable stream_end_cv_;
		std::jthread tail_drain_worker_;

		bool PublishEqualizerSnapshotLocked() noexcept;
		void CreateSourceVoice();
		void DestroySourceVoice() noexcept;
		BufferSlot* AcquireBuffer(std::size_t byte_count);
		bool SubmitBufferLocked(
			std::span<const std::uint8_t> bytes,
			std::size_t byte_count,
			std::uint32_t frame_count,
			AudioStreamGeneration generation,
			bool end_of_stream,
			bool tail_probe,
			std::uint64_t tail_probe_process_sequence,
			bool media_frames);
		bool BeginTailDrainLocked(AudioStreamGeneration generation);
		void TailDrainWorker(std::stop_token stop_token) noexcept;
		void UnaccountBufferFramesLocked(BufferSlot* slot) noexcept;
		void ReleaseBufferLocked(BufferSlot* slot) noexcept;
		void RecycleBuffer(
			BufferSlot* slot,
			bool defer_eos_until_stream_end = false) noexcept;
		bool AbortStreamLocked() noexcept;
		void ResetStreamProgressState() noexcept;
		void HandleStreamEnd() noexcept;
		void HandleVoiceError(
			std::uint32_t error,
			AudioStreamGeneration callback_generation) noexcept;
		struct VoiceProgressSnapshot
		{
			std::uint32_t buffers_queued = 0;
			std::uint64_t submitted_media_frames = 0;
			std::uint64_t mixed_media_frames = 0;
		};
		[[nodiscard]] VoiceProgressSnapshot QueryVoiceProgress() const noexcept;
		void RecordPlaybackClockPoint(
			AudioStreamGeneration generation,
			std::int64_t captured_at_nanoseconds,
			std::uint64_t mixed_media_frames) noexcept;
		void CapturePlaybackClockPoint() noexcept;
		[[nodiscard]] bool ReadPlaybackClockAt(
			AudioStreamGeneration generation,
			std::int64_t presentation_time_nanoseconds,
			std::uint64_t& media_frames) const noexcept;

		static void FAUDIOCALL OnBufferEnd(
			FAudioVoiceCallback* callback,
			void* buffer_context);
		static void FAUDIOCALL OnStreamEnd(FAudioVoiceCallback* callback);
		static void FAUDIOCALL OnVoiceProcessingPassEnd(
			FAudioVoiceCallback* callback);
		static void FAUDIOCALL OnVoiceError(
			FAudioVoiceCallback* callback,
			void* buffer_context,
			std::uint32_t error);

	public:
		explicit FAudioSink(
			const AudioOutputFormat& requested = {},
			std::shared_ptr<FAudioOutputDevice> device = nullptr);
		~FAudioSink() override;

		FAudioSink(const FAudioSink&) = delete;
		FAudioSink& operator=(const FAudioSink&) = delete;

		[[nodiscard]] const AudioOutputFormat& GetOutputFormat() const noexcept override;
		[[nodiscard]] const AudioOutputFormat& GetDeviceFormat() const noexcept override;
		[[nodiscard]] bool IsInitialized() const noexcept override;
		[[nodiscard]] bool IsLimiterEnabled() const noexcept override;
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

		[[nodiscard]] std::shared_ptr<FAudioOutputDevice> GetDevice() const noexcept
		{
			return device_;
		}

		bool IsExclusiveModeEnabled() noexcept override;
	};
}
