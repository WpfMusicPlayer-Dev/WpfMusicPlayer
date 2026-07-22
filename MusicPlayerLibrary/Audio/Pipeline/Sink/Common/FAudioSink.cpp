// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "Audio/Pipeline/Sink/Common/FapoEqualizer.h"
#include "Audio/Pipeline/Sink/Common/FAudioSink.h"
#include "Core/ChronoUtilities.h"
#include "Core/NumericConversion.h"

namespace
{
	constexpr std::uint32_t TailBarrierFrames = 1;
	constexpr std::uint32_t TailDrainChunkFrames = 256;
	constexpr std::uint64_t TailBarrierProcessSequence =
		(std::numeric_limits<std::uint64_t>::max)();
	constexpr auto BufferDrainTimeout = std::chrono::seconds(2);
	constexpr auto PresentationWaitPollInterval = std::chrono::milliseconds(2);

	std::uint64_t PublishMonotonic(
		std::atomic<std::uint64_t>& destination,
		const std::uint64_t candidate) noexcept
	{
		auto current = destination.load(std::memory_order_acquire);
		while (candidate > current && !destination.compare_exchange_weak(
			current, candidate, std::memory_order_acq_rel, std::memory_order_acquire))
		{
		}
		return (std::max)(current, candidate);
	}
}

MusicPlayerLibrary::FAudioSink::FAudioSink(
	const AudioOutputFormat& requested,
	std::shared_ptr<FAudioOutputDevice> device) :
	device_(device ? std::move(device) : FAudioOutputDevice::Acquire(requested))
{
	if (!device_ || !device_->GetEngine())
		throw std::runtime_error("FAudio output device is not initialized");
	// Source voices may use different normalized formats while every sink still
	// routes through the same engine and mastering voice.
	output_format_ = device_->ResolveSinkFormat(requested);
	for (std::size_t index = 0; index < BufferPoolSize; ++index)
	{
		buffer_slots_[index] = std::make_unique<BufferSlot>();
		free_buffers_[index] = buffer_slots_[index].get();
	}
	free_buffer_count_ = BufferPoolSize;
	CreateSourceVoice();
	try
	{
		tail_drain_worker_ = std::jthread(
			[this](const std::stop_token stop_token)
			{
				TailDrainWorker(stop_token);
			});
	}
	catch (...)
	{
		DestroySourceVoice();
		throw;
	}
}

void MusicPlayerLibrary::FAudioSink::CreateSourceVoice()
{
	voice_callback_.owner = this;
	voice_callback_.callbacks.OnBufferEnd = &FAudioSink::OnBufferEnd;
	voice_callback_.callbacks.OnStreamEnd = &FAudioSink::OnStreamEnd;
	voice_callback_.callbacks.OnVoiceError = &FAudioSink::OnVoiceError;
	voice_callback_.callbacks.OnVoiceProcessingPassEnd =
		&FAudioSink::OnVoiceProcessingPassEnd;

	FAudioSourceVoice* raw_source_voice = nullptr;
	const auto create_voice_result = FAudio_CreateSourceVoice(
		device_->GetEngine(),
		&raw_source_voice,
		&output_format_.wave_format.Format,
		FAUDIO_VOICE_NOPITCH,
		2.0f,
		&voice_callback_.callbacks,
		nullptr,
		nullptr);
	UniqueFAudioVoice source_voice(raw_source_voice);
	if (create_voice_result != FAUDIO_OK || !source_voice)
	{
		throw std::runtime_error("FAudio_CreateSourceVoice failed");
	}
	source_voice_ = std::move(source_voice);

	AudioDsp::EqualizerDspSnapshot initial_snapshot;
	{
		std::lock_guard effect_lock(effect_mutex_);
		initial_snapshot = equalizer_settings_.Compile(output_format_.sample_rate);
	}
	const auto limiter = AudioDsp::MakeSinkLimiterConfig();
	const auto lookahead_frames = static_cast<std::uint32_t>(std::floor(
		static_cast<double>(output_format_.sample_rate) * limiter.lookahead_ms /
		1000.0));
	limiter_latency_frames_ = limiter.enabled && lookahead_frames > 0
		? lookahead_frames - 1
		: 0;
	FAPO* raw_equalizer = nullptr;
	const auto create_equalizer_result = AudioDsp::CreateEqualizerFapo(
		initial_snapshot, limiter, &raw_equalizer);
	AudioDsp::UniqueFapo equalizer(raw_equalizer);
	if (create_equalizer_result != FAUDIO_OK || !equalizer)
	{
		DestroySourceVoice();
		throw std::runtime_error("CreateEqualizerFapo failed");
	}

	FAudioEffectDescriptor effect_descriptor{
		.pEffect = equalizer.get(),
		.InitialState = 0,
		.OutputChannels = output_format_.wave_format.Format.nChannels
	};
	FAudioEffectChain effect_chain{
		.EffectCount = 1,
		.pEffectDescriptors = &effect_descriptor
	};
	const std::uint32_t effect_result =
		FAudioVoice_SetEffectChain(source_voice_.get(), &effect_chain);
	if (effect_result != FAUDIO_OK)
	{
		DestroySourceVoice();
		throw std::runtime_error("FAudioVoice_SetEffectChain failed");
	}
	// Keep the factory reference in addition to FAudio's effect-chain
	// reference. The control worker only reads atomics through this handle.
	equalizer_effect_ = std::move(equalizer);

	{
		std::lock_guard effect_lock(effect_mutex_);
		if (!PublishEqualizerSnapshotLocked())
		{
			DestroySourceVoice();
			throw std::runtime_error("FAudio equalizer prewarm failed");
		}
	}
}

void MusicPlayerLibrary::FAudioSink::DestroySourceVoice() noexcept
{
	if (source_voice_)
	{
		AbortStream();
		source_voice_.reset();
	}
	equalizer_effect_.reset();
	is_limiter_enabled_.store(false, std::memory_order_release);
	effect_latency_frames_.store(0, std::memory_order_release);
}

MusicPlayerLibrary::FAudioSink::BufferSlot*
MusicPlayerLibrary::FAudioSink::AcquireBuffer(const std::size_t byte_count)
{
	BufferSlot* slot = nullptr;
	{
		std::lock_guard lock(buffer_mutex_);
		if (free_buffer_count_ == 0)
			return nullptr;
		slot = free_buffers_[--free_buffer_count_];
		slot->in_use = true;
		slot->frames_accounted = false;
		slot->descriptor = {};
		slot->frame_count = 0;
		slot->generation = 0;
		slot->end_of_stream = false;
		slot->tail_probe = false;
		slot->tail_probe_serial = 0;
		slot->tail_probe_process_sequence = 0;
		++in_flight_count_;
	}
	try
	{
		slot->storage.resize(byte_count);
	}
	catch (...)
	{
		RecycleBuffer(slot, false);
		throw;
	}
	return slot;
}

bool MusicPlayerLibrary::FAudioSink::SubmitBufferLocked(
	const std::span<const std::uint8_t> bytes,
	const std::size_t byte_count,
	const std::uint32_t frame_count,
	const AudioStreamGeneration buffer_generation,
	const bool end_of_stream,
	const bool tail_probe,
	const std::uint64_t tail_probe_process_sequence,
	const bool media_frames)
{
	if (!source_voice_ || byte_count == 0 || frame_count == 0 ||
		byte_count > (std::numeric_limits<std::uint32_t>::max)() ||
		(!bytes.empty() && bytes.size() != byte_count))
	{
		return false;
	}

	BufferSlot* slot = nullptr;
	try
	{
		slot = AcquireBuffer(byte_count);
		if (!slot)
			return false;
		if (bytes.empty())
			std::fill(slot->storage.begin(), slot->storage.end(), std::uint8_t{0});
		else
			std::memcpy(slot->storage.data(), bytes.data(), byte_count);
	}
	catch (...)
	{
		if (slot)
			RecycleBuffer(slot, false);
		return false;
	}

	slot->frame_count = frame_count;
	slot->generation = buffer_generation;
	slot->end_of_stream = end_of_stream;
	slot->tail_probe = tail_probe;
	if (tail_probe)
	{
		slot->tail_probe_serial = ++next_tail_probe_serial_;
		slot->tail_probe_process_sequence = tail_probe_process_sequence;
	}
	slot->descriptor.Flags = end_of_stream ? FAUDIO_END_OF_STREAM : 0;
	slot->descriptor.AudioBytes = static_cast<std::uint32_t>(byte_count);
	slot->descriptor.pAudioData = slot->storage.data();
	slot->descriptor.pContext = slot;

	{
		std::lock_guard lock(buffer_mutex_);
		queued_frames_ += slot->frame_count;
		slot->frames_accounted = true;
	}
	if (media_frames)
		submitted_media_frames_.fetch_add(frame_count, std::memory_order_release);
	if (FAudioSourceVoice_SubmitSourceBuffer(
		source_voice_.get(), &slot->descriptor, nullptr) != FAUDIO_OK)
	{
		if (media_frames)
			submitted_media_frames_.fetch_sub(frame_count, std::memory_order_release);
		RecycleBuffer(slot, false);
		return false;
	}
	return true;
}

bool MusicPlayerLibrary::FAudioSink::BeginTailDrainLocked(
	const AudioStreamGeneration stream_generation)
{
	if (!source_voice_ ||
		stream_phase_.load(std::memory_order_acquire) != StreamPhase::open ||
		stream_generation != generation_.load(std::memory_order_acquire))
	{
		return false;
	}

	const auto byte_count =
		static_cast<std::size_t>(output_format_.wave_format.Format.nBlockAlign) *
		TailBarrierFrames;
	if (!is_limiter_enabled_.load(std::memory_order_acquire))
	{
		stream_phase_.store(StreamPhase::eos_queued, std::memory_order_release);
		if (SubmitBufferLocked(
			{}, byte_count, TailBarrierFrames, stream_generation,
			true, false, 0, false))
		{
			return true;
		}
		stream_phase_.store(StreamPhase::open, std::memory_order_release);
		return false;
	}
	if (!equalizer_effect_)
		return false;

	stream_phase_.store(StreamPhase::draining, std::memory_order_release);
	if (SubmitBufferLocked(
		{}, byte_count, TailBarrierFrames, stream_generation,
		false, true, TailBarrierProcessSequence, false))
	{
		return true;
	}

	stream_phase_.store(StreamPhase::open, std::memory_order_release);
	return false;
}

void MusicPlayerLibrary::FAudioSink::TailDrainWorker(
	const std::stop_token stop_token) noexcept
{
	std::uint64_t handled_serial = 0;
	while (!stop_token.stop_requested())
	{
		AudioStreamGeneration probe_generation = 0;
		std::uint64_t probe_process_sequence = 0;
		{
			std::unique_lock lock(buffer_mutex_);
			buffer_cv_.wait(lock, [this, &stop_token, handled_serial]
				{
					return stop_token.stop_requested() ||
						completed_tail_probe_serial_ > handled_serial;
				});
			if (stop_token.stop_requested())
				return;
			handled_serial = completed_tail_probe_serial_;
			probe_generation = completed_tail_probe_generation_;
			probe_process_sequence = completed_tail_probe_process_sequence_;
		}

		try
		{
			if (probe_process_sequence != TailBarrierProcessSequence)
			{
				while (!stop_token.stop_requested() &&
					AudioDsp::EqualizerFapoProcessSequence(equalizer_effect_.get()) ==
						probe_process_sequence)
				{
					if (probe_generation !=
							generation_.load(std::memory_order_acquire) ||
						stream_phase_.load(std::memory_order_acquire) !=
							StreamPhase::draining)
					{
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (stop_token.stop_requested())
					return;
			}

			std::lock_guard submit_lock(submit_mutex_);
			if (probe_generation == 0 ||
				probe_generation != generation_.load(std::memory_order_acquire) ||
				stream_phase_.load(std::memory_order_acquire) != StreamPhase::draining)
			{
				continue;
			}

			// The first callback is only the source-buffer barrier. Always queue
			// one drain chunk after it; later callbacks are paired with a FAPO
			// process-sequence change before HasTail is sampled.
			const bool has_tail =
				probe_process_sequence == TailBarrierProcessSequence ||
				AudioDsp::EqualizerFapoHasTail(equalizer_effect_.get());
			const auto frame_count = has_tail
				? TailDrainChunkFrames
				: std::uint32_t{1};
			const auto byte_count =
				static_cast<std::size_t>(output_format_.wave_format.Format.nBlockAlign) *
				frame_count;
			if (!SubmitBufferLocked(
				{}, byte_count, frame_count, probe_generation,
				!has_tail, has_tail,
				has_tail
					? AudioDsp::EqualizerFapoProcessSequence(equalizer_effect_.get())
					: 0,
				false))
			{
				throw std::runtime_error("failed to submit equalizer tail drain buffer");
			}
			if (!has_tail)
				stream_phase_.store(StreamPhase::eos_queued, std::memory_order_release);
		}
		catch (...)
		{
			HandleVoiceError(FAUDIO_E_FAIL, 0);
		}
	}
}

void MusicPlayerLibrary::FAudioSink::UnaccountBufferFramesLocked(
	BufferSlot* slot) noexcept
{
	if (!slot || !slot->frames_accounted)
		return;
	queued_frames_ = queued_frames_ >= slot->frame_count
		? queued_frames_ - slot->frame_count
		: 0;
	slot->frames_accounted = false;
}

void MusicPlayerLibrary::FAudioSink::ReleaseBufferLocked(BufferSlot* slot) noexcept
{
	if (!slot || !slot->in_use)
		return;
	UnaccountBufferFramesLocked(slot);
	slot->in_use = false;
	if (free_buffer_count_ < BufferPoolSize)
		free_buffers_[free_buffer_count_++] = slot;
	if (in_flight_count_ > 0)
		--in_flight_count_;
}

void MusicPlayerLibrary::FAudioSink::RecycleBuffer(
	BufferSlot* slot,
	const bool defer_eos_until_stream_end) noexcept
{
	if (!slot)
		return;
	try
	{
		std::lock_guard lock(buffer_mutex_);
		if (!slot->in_use)
			return;
		if (slot->tail_probe)
		{
			completed_tail_probe_serial_ = (std::max)(
				completed_tail_probe_serial_, slot->tail_probe_serial);
			completed_tail_probe_generation_ = slot->generation;
			completed_tail_probe_process_sequence_ =
				slot->tail_probe_process_sequence;
		}
		if (defer_eos_until_stream_end && slot->end_of_stream &&
			!aborting_.load(std::memory_order_acquire))
		{
			UnaccountBufferFramesLocked(slot);
			pending_eos_callback_ = slot;
			return;
		}
		ReleaseBufferLocked(slot);
		buffer_cv_.notify_all();
	}
	catch (...)
	{
		HandleVoiceError(FAUDIO_E_FAIL, 0);
	}
}

void MusicPlayerLibrary::FAudioSink::HandleStreamEnd() noexcept
{
	AudioStreamGeneration engine_ended_generation = 0;
	std::int64_t engine_end_clock_nanoseconds = 0;
	std::uint64_t final_media_frames = 0;
	try
	{
		std::lock_guard lock(buffer_mutex_);
		BufferSlot* eos_slot = pending_eos_callback_;
		pending_eos_callback_ = nullptr;
		if (!eos_slot)
			return;
		const auto callback_generation = eos_slot->generation;
		const auto current_generation = generation_.load(std::memory_order_acquire);
		auto expected_phase = StreamPhase::eos_queued;
		if (callback_generation == current_generation &&
			stream_phase_.compare_exchange_strong(
				expected_phase, StreamPhase::engine_ended,
				std::memory_order_acq_rel))
		{
			engine_ended_generation = current_generation;
			engine_end_clock_nanoseconds = SteadyClockNowNanoseconds();
			final_media_frames = submitted_media_frames_.load(
				std::memory_order_acquire);
		}
		ReleaseBufferLocked(eos_slot);
		buffer_cv_.notify_all();
	}
	catch (...)
	{
		HandleVoiceError(FAUDIO_E_FAIL, 0);
	}
	if (engine_ended_generation != 0)
	{
		// FAudio resets SamplesPlayed before OnStreamEnd. Publish an explicit
		// final processing-pass point so the presentation clock still reaches
		// every submitted media frame after the device latency has elapsed.
		RecordPlaybackClockPoint(
			engine_ended_generation,
			engine_end_clock_nanoseconds,
			final_media_frames);
		engine_end_clock_nanoseconds_.store(
			engine_end_clock_nanoseconds, std::memory_order_release);
		engine_completed_generation_.store(
			engine_ended_generation, std::memory_order_release);
	}
	if (engine_ended_generation != 0)
		stream_end_cv_.notify_all();
}

void MusicPlayerLibrary::FAudioSink::HandleVoiceError(
	const std::uint32_t error,
	const AudioStreamGeneration callback_generation) noexcept
{
	if (callback_generation != 0 &&
		callback_generation != generation_.load(std::memory_order_acquire))
	{
		return;
	}
	voice_error_.store(error, std::memory_order_release);
	stream_phase_.store(StreamPhase::error, std::memory_order_release);
	stream_end_cv_.notify_all();
}

void MusicPlayerLibrary::FAudioSink::RecordPlaybackClockPoint(
	const AudioStreamGeneration point_generation,
	const std::int64_t captured_at_nanoseconds,
	const std::uint64_t mixed_media_frames) noexcept
{
	if (point_generation == 0 || captured_at_nanoseconds <= 0)
		return;
	const auto sequence = playback_clock_write_sequence_.fetch_add(
		1, std::memory_order_acq_rel) + 1;
	auto& point = playback_clock_[(sequence - 1) % PlaybackClockCapacity];
	// Invalidate first so a reader cannot combine the old sequence number with
	// fields from the point currently being overwritten.
	point.sequence.store(0, std::memory_order_release);
	point.generation.store(point_generation, std::memory_order_relaxed);
	point.captured_at_nanoseconds.store(
		captured_at_nanoseconds, std::memory_order_relaxed);
	point.mixed_media_frames.store(mixed_media_frames, std::memory_order_relaxed);
	point.sequence.store(sequence, std::memory_order_release);
}

MusicPlayerLibrary::FAudioSink::VoiceProgressSnapshot
MusicPlayerLibrary::FAudioSink::QueryVoiceProgress() const noexcept
{
	VoiceProgressSnapshot result{};
	if (!source_voice_)
		return result;

	FAudioVoiceState raw_state{};
	FAudioSourceVoice_GetState(source_voice_.get(), &raw_state, 0);
	const auto start_samples = session_start_samples_.load(std::memory_order_acquire);
	const auto raw_session_samples = raw_state.SamplesPlayed >= start_samples
		? raw_state.SamplesPlayed - start_samples
		: 0;
	result.buffers_queued = raw_state.BuffersQueued;
	result.submitted_media_frames = submitted_media_frames_.load(
		std::memory_order_acquire);
	result.mixed_media_frames = (std::min)(
		raw_session_samples, result.submitted_media_frames);
	return result;
}

void MusicPlayerLibrary::FAudioSink::CapturePlaybackClockPoint() noexcept
{
	const auto phase = stream_phase_.load(std::memory_order_acquire);
	if (!source_voice_ || (phase != StreamPhase::open &&
		phase != StreamPhase::draining && phase != StreamPhase::eos_queued))
	{
		return;
	}
	const auto point_generation = generation_.load(std::memory_order_acquire);
	if (point_generation == 0)
		return;

	const auto voice_progress = QueryVoiceProgress();

	// BeginStream publishes `open` only after its generation baseline is ready,
	// while AbortStream publishes `aborted` before flushing the voice. Recheck
	// both values after querying FAudio so a callback straddling either boundary
	// cannot stamp its samples into the next generation.
	if (generation_.load(std::memory_order_acquire) != point_generation)
		return;
	const auto confirmed_phase = stream_phase_.load(std::memory_order_acquire);
	if (confirmed_phase != StreamPhase::open &&
		confirmed_phase != StreamPhase::draining &&
		confirmed_phase != StreamPhase::eos_queued)
	{
		return;
	}
	RecordPlaybackClockPoint(
		point_generation, SteadyClockNowNanoseconds(),
		voice_progress.mixed_media_frames);
}

bool MusicPlayerLibrary::FAudioSink::ReadPlaybackClockAt(
	const AudioStreamGeneration requested_generation,
	const std::int64_t presentation_time_nanoseconds,
	std::uint64_t& media_frames) const noexcept
{
	const auto newest_sequence = playback_clock_write_sequence_.load(
		std::memory_order_acquire);
	const auto point_count = (std::min)(
		newest_sequence, static_cast<std::uint64_t>(PlaybackClockCapacity));
	for (std::uint64_t offset = 0; offset < point_count; ++offset)
	{
		const auto sequence = newest_sequence - offset;
		const auto& point = playback_clock_[(sequence - 1) % PlaybackClockCapacity];
		if (point.sequence.load(std::memory_order_acquire) != sequence)
			continue;
		const auto point_generation = point.generation.load(std::memory_order_relaxed);
		const auto captured_at = point.captured_at_nanoseconds.load(
			std::memory_order_relaxed);
		const auto point_frames = point.mixed_media_frames.load(
			std::memory_order_relaxed);
		if (point.sequence.load(std::memory_order_acquire) != sequence)
			continue;
		if (point_generation == requested_generation &&
			captured_at <= presentation_time_nanoseconds)
		{
			media_frames = point_frames;
			return true;
		}
	}
	return false;
}

void FAUDIOCALL MusicPlayerLibrary::FAudioSink::OnBufferEnd(
	FAudioVoiceCallback* callback,
	void* buffer_context)
{
	auto* context = reinterpret_cast<VoiceCallbackContext*>(callback);
	if (context && context->owner)
		context->owner->RecycleBuffer(
			static_cast<BufferSlot*>(buffer_context), true);
}

void FAUDIOCALL MusicPlayerLibrary::FAudioSink::OnStreamEnd(
	FAudioVoiceCallback* callback)
{
	auto* context = reinterpret_cast<VoiceCallbackContext*>(callback);
	if (context && context->owner)
		context->owner->HandleStreamEnd();
}

void FAUDIOCALL MusicPlayerLibrary::FAudioSink::OnVoiceProcessingPassEnd(
	FAudioVoiceCallback* callback)
{
	auto* context = reinterpret_cast<VoiceCallbackContext*>(callback);
	if (context && context->owner)
		context->owner->CapturePlaybackClockPoint();
}

void FAUDIOCALL MusicPlayerLibrary::FAudioSink::OnVoiceError(
	FAudioVoiceCallback* callback,
	void* buffer_context,
	const std::uint32_t error)
{
	auto* context = reinterpret_cast<VoiceCallbackContext*>(callback);
	if (context && context->owner)
	{
		auto* slot = static_cast<BufferSlot*>(buffer_context);
		const auto callback_generation = slot ? slot->generation : 0;
		context->owner->RecycleBuffer(slot, false);
		context->owner->HandleVoiceError(error, callback_generation);
	}
}

bool MusicPlayerLibrary::FAudioSink::PublishEqualizerSnapshotLocked() noexcept
{
	if (!source_voice_)
		return false;
	const bool currently_enabled = is_limiter_enabled_.load(
		std::memory_order_acquire);
	const auto publication = equalizer_settings_.PreparePublication(
		output_format_.sample_rate, false, currently_enabled);
	const auto& snapshot = publication.snapshot;
	const bool should_enable = publication.processing_enabled;
	if (FAudioVoice_SetEffectParameters(
			source_voice_.get(), 0, &snapshot, sizeof(snapshot), FAUDIO_COMMIT_NOW) != FAUDIO_OK)
	{
		return false;
	}

	if (should_enable != currently_enabled)
	{
		const auto result = should_enable
			? FAudioVoice_EnableEffect(
				source_voice_.get(), 0, FAUDIO_COMMIT_NOW)
			: FAudioVoice_DisableEffect(
				source_voice_.get(), 0, FAUDIO_COMMIT_NOW);
		if (result != FAUDIO_OK)
			return false;
	}

	is_limiter_enabled_.store(should_enable, std::memory_order_release);
	effect_latency_frames_.store(
		should_enable ? limiter_latency_frames_ : 0,
		std::memory_order_release);
	return true;
}

const MusicPlayerLibrary::AudioOutputFormat&
MusicPlayerLibrary::FAudioSink::GetOutputFormat() const noexcept
{
	return output_format_;
}

const MusicPlayerLibrary::AudioOutputFormat&
MusicPlayerLibrary::FAudioSink::GetDeviceFormat() const noexcept
{
	return device_->GetOutputFormat();
}

bool MusicPlayerLibrary::FAudioSink::IsInitialized() const noexcept
{
	return device_ && device_->GetEngine() && source_voice_;
}

bool MusicPlayerLibrary::FAudioSink::IsLimiterEnabled() const noexcept
{
	return is_limiter_enabled_.load(std::memory_order_acquire);
}

MusicPlayerLibrary::AudioStreamGeneration
MusicPlayerLibrary::FAudioSink::BeginStream()
{
	std::lock_guard submit_lock(submit_mutex_);
	if (!AbortStreamLocked())
		throw std::runtime_error("FAudio source voice did not drain during reset");
	const auto next_generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
	ResetStreamProgressState();
	submitted_media_frames_.store(0, std::memory_order_release);
	voice_error_.store(0, std::memory_order_release);
	FAudioVoiceState raw_state{};
	if (source_voice_)
		FAudioSourceVoice_GetState(source_voice_.get(), &raw_state, 0);
	session_start_samples_.store(raw_state.SamplesPlayed, std::memory_order_release);
	{
		std::lock_guard effect_lock(effect_mutex_);
		equalizer_settings_.AdvanceResetGeneration();
		(void)PublishEqualizerSnapshotLocked();
	}
	// Publish open only after the generation clock baseline and effect reset are
	// complete, so concurrent state readers cannot observe a half-open epoch.
	stream_phase_.store(StreamPhase::open, std::memory_order_release);
	return next_generation;
}

bool MusicPlayerLibrary::FAudioSink::Submit(const NormalizedPcmBlock& block)
{
	std::lock_guard submit_lock(submit_mutex_);
	if (!source_voice_ || block.bytes.empty() || block.frame_count == 0 ||
		block.generation != generation_.load(std::memory_order_acquire))
	{
		return false;
	}
	const auto phase = stream_phase_.load(std::memory_order_acquire);
	if (phase != StreamPhase::open)
		return false;
	const bool direct_eos = block.end_of_stream &&
		!is_limiter_enabled_.load(std::memory_order_acquire);
	if (direct_eos)
		stream_phase_.store(StreamPhase::eos_queued, std::memory_order_release);
	if (!SubmitBufferLocked(
		block.bytes, block.bytes.size(), block.frame_count, block.generation,
		direct_eos, false, 0, true))
	{
		if (direct_eos)
			stream_phase_.store(StreamPhase::open, std::memory_order_release);
		return false;
	}
	if (direct_eos)
		return true;
	return !block.end_of_stream || BeginTailDrainLocked(block.generation);
}

bool MusicPlayerLibrary::FAudioSink::EndStream() noexcept
{
	try
	{
		std::lock_guard submit_lock(submit_mutex_);
		return BeginTailDrainLocked(
			generation_.load(std::memory_order_acquire));
	}
	catch (...)
	{
		return false;
	}
}

bool MusicPlayerLibrary::FAudioSink::Start() noexcept
{
	try
	{
		std::lock_guard submit_lock(submit_mutex_);
		const auto phase = stream_phase_.load(std::memory_order_acquire);
		return source_voice_ &&
			(phase == StreamPhase::open || phase == StreamPhase::draining ||
				phase == StreamPhase::eos_queued) &&
			FAudioSourceVoice_Start(
				source_voice_.get(), 0, FAUDIO_COMMIT_NOW) == FAUDIO_OK;
	}
	catch (...)
	{
		return false;
	}
}

void MusicPlayerLibrary::FAudioSink::Stop() noexcept
{
	if (source_voice_)
		(void)FAudioSourceVoice_Stop(
			source_voice_.get(), 0, FAUDIO_COMMIT_NOW);
}

void MusicPlayerLibrary::FAudioSink::ResetStreamProgressState() noexcept
{
	engine_completed_generation_.store(0, std::memory_order_release);
	completed_generation_.store(0, std::memory_order_release);
	engine_end_clock_nanoseconds_.store(0, std::memory_order_release);
	last_samples_played_.store(0, std::memory_order_release);
	last_media_frames_presented_.store(0, std::memory_order_release);
}

bool MusicPlayerLibrary::FAudioSink::AbortStreamLocked() noexcept
{
	if (!source_voice_)
		return true;
	aborting_.store(true, std::memory_order_release);
	try
	{
		stream_phase_.store(StreamPhase::aborted, std::memory_order_release);
		Stop();
		(void)FAudioSourceVoice_FlushSourceBuffers(source_voice_.get());
		std::unique_lock lock(buffer_mutex_);
		if (pending_eos_callback_)
		{
			ReleaseBufferLocked(pending_eos_callback_);
			pending_eos_callback_ = nullptr;
			buffer_cv_.notify_all();
		}
		const bool drained = buffer_cv_.wait_for(lock, BufferDrainTimeout, [this]
			{
				return in_flight_count_ == 0;
			});
		if (!drained)
		{
			HandleVoiceError(FAUDIO_E_FAIL, 0);
			return false;
		}
		completed_tail_probe_serial_ = 0;
		completed_tail_probe_generation_ = 0;
		completed_tail_probe_process_sequence_ = 0;
		ResetStreamProgressState();
		aborting_.store(false, std::memory_order_release);
		stream_end_cv_.notify_all();
		return true;
	}
	catch (...)
	{
		HandleVoiceError(FAUDIO_E_FAIL, 0);
		return false;
	}
}

void MusicPlayerLibrary::FAudioSink::AbortStream() noexcept
{
	try
	{
		std::lock_guard submit_lock(submit_mutex_);
		(void)AbortStreamLocked();
	}
	catch (...)
	{
		HandleVoiceError(FAUDIO_E_FAIL, 0);
	}
}

MusicPlayerLibrary::AudioSinkState
MusicPlayerLibrary::FAudioSink::GetState() const noexcept
{
	// BeginStream and AbortStream both replace generation-scoped clock state.
	// Serialize the complete snapshot so no result combines two generations.
	std::lock_guard submit_lock(submit_mutex_);
	AudioSinkState result{};
	result.generation = generation_.load(std::memory_order_acquire);
	result.error_code = voice_error_.load(std::memory_order_acquire);
	const auto phase = stream_phase_.load(std::memory_order_acquire);
	if (!source_voice_ || phase == StreamPhase::idle ||
		phase == StreamPhase::aborted)
		return result;
	if (result.generation != 0 &&
		completed_generation_.load(std::memory_order_acquire) == result.generation)
	{
		result.stream_ended = true;
		return result;
	}

	const auto voice_progress = QueryVoiceProgress();
	result.buffers_queued = voice_progress.buffers_queued;
	const auto submitted_media_frames = voice_progress.submitted_media_frames;
	result.samples_played = PublishMonotonic(
		last_samples_played_, voice_progress.mixed_media_frames);
	const auto device_rate = device_->GetOutputFormat().sample_rate;
	const auto device_latency = device_->GetCurrentLatencyInSamples();
	const auto converted_device_latency = device_rate > 0
		? static_cast<std::uint64_t>(
			(static_cast<std::uint64_t>(device_latency) * output_format_.sample_rate +
				device_rate - 1) / device_rate)
		: 0;
	const auto effect_latency_frames = effect_latency_frames_.load(
		std::memory_order_acquire);
	const auto presentation_latency = converted_device_latency +
		effect_latency_frames;
	result.presentation_latency_frames = SaturateToUint32(presentation_latency);
	// Select an actual FAudio processing-pass sample in time instead of deriving
	// delay from source queue depth. This remains correct across prefetch bursts,
	// pauses and underruns because no query-time points are inserted.
	const double latency_seconds =
		(device_rate > 0
			? static_cast<double>(device_latency) / device_rate
			: 0.0) +
		(output_format_.sample_rate > 0
			? static_cast<double>(effect_latency_frames) /
				output_format_.sample_rate
			: 0.0);
	const auto latency_nanoseconds =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(latency_seconds)).count();
	const auto presentation_time_nanoseconds =
		SteadyClockNowNanoseconds() - latency_nanoseconds;
	std::uint64_t presented_media_frames = 0;
	(void)ReadPlaybackClockAt(
		result.generation,
		presentation_time_nanoseconds,
		presented_media_frames);
	result.media_frames_presented = PublishMonotonic(
		last_media_frames_presented_,
		(std::min)(presented_media_frames, submitted_media_frames));
	{
		std::lock_guard lock(buffer_mutex_);
		result.queued_frames = queued_frames_;
	}
	const bool engine_ended = result.generation != 0 &&
		engine_completed_generation_.load(std::memory_order_acquire) ==
			result.generation;
	if (engine_ended)
		result.samples_played = (std::max)(result.samples_played, submitted_media_frames);
	const auto engine_end_clock_nanoseconds =
		engine_end_clock_nanoseconds_.load(std::memory_order_acquire);
	if (engine_ended && engine_end_clock_nanoseconds > 0 &&
		presentation_time_nanoseconds >= engine_end_clock_nanoseconds)
	{
		// The EOS marker has passed both the effect and device latency. Only now
		// is the generation presentation-complete and safe to reset publicly.
		result.media_frames_presented = PublishMonotonic(
			last_media_frames_presented_, submitted_media_frames);
		completed_generation_.store(result.generation, std::memory_order_release);
		auto expected_phase = StreamPhase::engine_ended;
		(void)stream_phase_.compare_exchange_strong(
			expected_phase, StreamPhase::ended, std::memory_order_acq_rel);
		stream_end_cv_.notify_all();
	}
	result.stream_ended = result.generation != 0 &&
		completed_generation_.load(std::memory_order_acquire) == result.generation;
	if (result.stream_ended)
	{
		result.buffers_queued = 0;
		result.samples_played = 0;
		result.media_frames_presented = 0;
		result.presentation_latency_frames = 0;
		result.queued_frames = 0;
	}
	return result;
}

bool MusicPlayerLibrary::FAudioSink::WaitForStreamEnd(
	const std::chrono::milliseconds timeout)
{
	AudioStreamGeneration expected_generation = 0;
	{
		std::lock_guard submit_lock(submit_mutex_);
		expected_generation = generation_.load(std::memory_order_acquire);
	}
	if (expected_generation == 0)
		return false;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (;;)
	{
		const auto state = GetState();
		if (state.generation != expected_generation)
			return false;
		if (state.stream_ended || state.error_code != 0 ||
			stream_phase_.load(std::memory_order_acquire) == StreamPhase::aborted)
		{
			return true;
		}
		const auto now = std::chrono::steady_clock::now();
		if (timeout <= std::chrono::milliseconds::zero() || now >= deadline)
			return false;
		std::unique_lock lock(stream_end_mutex_);
		stream_end_cv_.wait_until(
			lock, (std::min)(deadline, now + PresentationWaitPollInterval));
	}
}

void MusicPlayerLibrary::FAudioSink::SetMasterVolume(const float volume) noexcept
{
	if (device_)
		device_->SetMasterVolume(volume);
}

int MusicPlayerLibrary::FAudioSink::GetEqualizerBand(const int index) const noexcept
{
	std::lock_guard effect_lock(effect_mutex_);
	return equalizer_settings_.GetBand(index);
}

void MusicPlayerLibrary::FAudioSink::SetEqualizerBand(int index, int value) noexcept
{
	std::lock_guard submit_lock(submit_mutex_);
	std::lock_guard effect_lock(effect_mutex_);
	if (!equalizer_settings_.SetBand(index, value))
		return;
	const auto phase = stream_phase_.load(std::memory_order_acquire);
	// Once EOS draining starts, changing the effect's enabled state can prevent
	// the FAPO process-sequence barrier from ever advancing. Keep the effect
	// state for this generation and publish the updated controls at BeginStream.
	if (phase == StreamPhase::draining || phase == StreamPhase::eos_queued)
		return;
	(void)PublishEqualizerSnapshotLocked();
}

bool MusicPlayerLibrary::FAudioSink::IsExclusiveModeEnabled() noexcept
{
	return false;
}

MusicPlayerLibrary::FAudioSink::~FAudioSink()
{
	if (tail_drain_worker_.joinable())
	{
		tail_drain_worker_.request_stop();
		buffer_cv_.notify_all();
		tail_drain_worker_.join();
	}
	DestroySourceVoice();
	voice_callback_.owner = nullptr;
}
