// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/Pipeline/Observer/FFTAudioObserve.h"
#include "Audio/FFT/FFTExecuter.h"

MusicPlayerLibrary::FFTAudioObserve::FFTAudioObserve(
	const AudioOutputFormat& format) :
	format_(format),
	executer_(std::make_unique<FFTExecuter>(format))
{
}

MusicPlayerLibrary::FFTAudioObserve::~FFTAudioObserve() = default;

void MusicPlayerLibrary::FFTAudioObserve::OnFormat(
	const AudioOutputFormat& format,
	const AudioStreamGeneration generation)
{
	std::lock_guard lock(mutex_);
	if (generation < generation_)
		return;
	if (AreResolvedPcmFormatsEqual(format, format_))
	{
		// Reset before publishing the new generation. CopySpectrum takes the
		// same lock, so it can never observe a new generation paired with the
		// preceding generation's spectrum timeline.
		if (generation > generation_ && executer_)
			executer_->ResetBuffers();
	}
	else
	{
		format_ = format;
		executer_ = std::make_unique<FFTExecuter>(format_);
	}
	generation_ = generation;
}

void MusicPlayerLibrary::FFTAudioObserve::OnPcm(const NormalizedPcmBlock& block)
{
	std::lock_guard lock(mutex_);
	if (block.generation == generation_ && executer_ &&
		!block.bytes.empty() && block.frame_count > 0)
	{
		const double stream_position_seconds = format_.sample_rate > 0
			? static_cast<double>(block.stream_frame_offset) / format_.sample_rate
			: 0.0;
		executer_->AddSamplesToRingBuffer(
			block.bytes.data(), block.frame_count, stream_position_seconds);
	}
}

void MusicPlayerLibrary::FFTAudioObserve::OnReset(
	const AudioStreamGeneration generation)
{
	std::lock_guard lock(mutex_);
	if (generation < generation_)
		return;
	generation_ = generation;
	if (executer_)
		executer_->ResetBuffers();
}

void MusicPlayerLibrary::FFTAudioObserve::OnEndOfStream(
	const AudioStreamGeneration generation)
{
	std::lock_guard lock(mutex_);
	if (generation != generation_)
		return;
}

int MusicPlayerLibrary::FFTAudioObserve::CopySpectrum(
	float* destination,
	const int destination_length,
	const AudioStreamGeneration generation,
	const std::uint64_t media_frames_presented) const
{
	std::lock_guard lock(mutex_);
	return executer_ && generation == generation_ && format_.sample_rate > 0
		? executer_->CopyAudioFFTDataAt(
			destination,
			destination_length,
			static_cast<double>(media_frames_presented) / format_.sample_rate)
		: 0;
}
