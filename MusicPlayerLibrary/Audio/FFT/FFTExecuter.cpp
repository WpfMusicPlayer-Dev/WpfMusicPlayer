// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "Audio/FFmpeg/FFmpegError.h"
#include "Audio/FFT/FFTExecuter.h"
#include "Core/BinaryData.h"

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#if defined(__cplusplus)
}
#endif

std::vector<uint8_t> MusicPlayerLibrary::FFTExecuter::ResampleToFftFormat(
	const uint8_t* interleaved_samples,
	const int frame_count)
{
	if (!interleaved_samples || frame_count <= 0)
		return {};

	std::lock_guard resample_lock(resample_mutex);
	if (!resample_context)
		return {};

	const int output_capacity = swr_get_out_samples(
		resample_context.get(), frame_count);
	if (output_capacity < 0)
	{
		NATIVE_TRACE("err: query FFT resample output size failed, reason=%s\n",
			GetFFmpegErrorMessage(output_capacity).c_str());
		return {};
	}
	if (output_capacity == 0)
		return {};

	std::vector<uint8_t> result(
		static_cast<size_t>(output_capacity) * BytesPerFrame);
	const uint8_t* input_planes[] = { interleaved_samples };
	uint8_t* output_planes[] = { result.data() };
	const int converted_frames = swr_convert(
		resample_context.get(),
		output_planes,
		output_capacity,
		input_planes,
		frame_count);
	if (converted_frames < 0)
	{
		NATIVE_TRACE("err: FFT resample failed, reason=%s\n",
			GetFFmpegErrorMessage(converted_frames).c_str());
		return {};
	}

	result.resize(static_cast<size_t>(converted_frames) * BytesPerFrame);
	return result;
}

void MusicPlayerLibrary::FFTExecuter::AddSamplesToRingBuffer(
	const uint8_t* interleaved_samples,
	const int frame_count,
	const double pts_seconds)
{
	if (!interleaved_samples || frame_count <= 0 ||
		!std::isfinite(pts_seconds) || input_sample_rate <= 0)
	{
		return;
	}

	const double input_duration = static_cast<double>(frame_count) /
		input_sample_rate;
	bool discontinuity = false;
	{
		std::lock_guard ring_buffer_lock(ring_buffer_mutex);
		// Allow sub-sample rounding differences but reset on seeks or dropped
		// source spans so a window never straddles two media epochs.
		const double tolerance = (std::max)(
			2.0 / input_sample_rate, 0.001);
		discontinuity = ring_timeline_initialized &&
			std::abs(pts_seconds - next_input_pts_seconds) > tolerance;
	}
	if (discontinuity)
		ResetBuffers();

	std::vector<uint8_t> samples =
		ResampleToFftFormat(interleaved_samples, frame_count);

	std::lock_guard ring_buffer_lock(ring_buffer_mutex);
	if (!ring_timeline_initialized)
	{
		ring_buffer_start_pts_seconds = pts_seconds;
		ring_timeline_initialized = true;
	}
	next_input_pts_seconds = pts_seconds + input_duration;
	for (const uint8_t sample : samples)
	{
		spectrum_data_ring_buffer.push_back(sample);
	}

	// A slow observer must not create unbounded memory growth. Drop complete
	// media hops while retaining enough samples for the next FFT window.
	if (spectrum_data_ring_buffer.size() > RingBufferCapacityBytes)
	{
		const std::size_t excess_frames =
			(spectrum_data_ring_buffer.size() - RingBufferCapacityBytes +
				BytesPerFrame - 1) / BytesPerFrame;
		const std::size_t drop_frames =
			((excess_frames + FftHopFrames - 1) / FftHopFrames) *
			FftHopFrames;
		const std::size_t drop_bytes = (std::min)(
			drop_frames * BytesPerFrame,
			spectrum_data_ring_buffer.size());
		for (std::size_t index = 0; index < drop_bytes; ++index)
			spectrum_data_ring_buffer.pop_front();
		ring_buffer_start_pts_seconds +=
			static_cast<double>(drop_bytes / BytesPerFrame) /
			StandardAudioSampleRate;
	}
	ring_buffer_has_unprocessed_data =
		spectrum_data_ring_buffer.size() >= RingBufferWindowBytes;

	// wake fft consumer thread
	if (ring_buffer_has_unprocessed_data)
		ring_buffer_cv.notify_one();
}

int MusicPlayerLibrary::FFTExecuter::GetRingBufferSize() const
{
	std::lock_guard lock(ring_buffer_mutex);
	return static_cast<int>(spectrum_data_ring_buffer.size());
}

void MusicPlayerLibrary::FFTExecuter::ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output)
{
	const size_t frame_count = input.size() / BytesPerFrame;
	output.resize(frame_count);

	for (size_t i = 0; i < frame_count; ++i) {
		const size_t frame_offset = i * BytesPerFrame;
		const auto left = static_cast<std::int16_t>(
			DecodeLittleEndian<std::uint16_t>(
				std::span<const std::uint8_t, sizeof(std::uint16_t)>(
					input.data() + frame_offset, sizeof(std::uint16_t))));
		const auto right = static_cast<std::int16_t>(
			DecodeLittleEndian<std::uint16_t>(
				std::span<const std::uint8_t, sizeof(std::uint16_t)>(
					input.data() + frame_offset + sizeof(std::uint16_t),
					sizeof(std::uint16_t))));
		// mix 2 channels
		double sample = (static_cast<double>(left) + static_cast<double>(right)) / 2.0;
		// hamming window
		const double w = 0.53836 * (1.0 - cos(2.0 * std::numbers::pi * i / (frame_count - 1)));
		// normalize
		output[i] = (sample / 32768.0) * w;
	}
}

void MusicPlayerLibrary::FFTExecuter::DoFFT(const std::vector<double>& windowed_data, std::vector<float>& fft_result, kiss_fft_cfg fft_cfg)
{
	size_t n = windowed_data.size();
	if (n == 0) return;

	// padding imaginary part to zero
	// cause kissfft only supports real input
	for (size_t i = 0; i < n; ++i) {
		fft_in[i].r = windowed_data[i];
		fft_in[i].i = 0.0f;
	}
	// zero padding
	for (size_t i = n; i < FftSize; ++i) {
		fft_in[i].r = 0.0;
		fft_in[i].i = 0.0;
	}

	kiss_fft(fft_cfg, fft_in.data(), fft_out.data());

	// Magnitude spectrum.
	fft_result.resize(FftSize / 2);
	for (size_t i = 0; i < FftSize / 2; ++i) {
		float real = fft_out[i].r;
		float imag = fft_out[i].i;
		fft_result[i] = sqrtf(real * real + imag * imag);
	}
}

std::vector<size_t> MusicPlayerLibrary::FFTExecuter::GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo, float f_hi)
{
	std::vector<size_t> boundaries(segment_num + 1);
	float delta_f = sample_rate / fft_size;
	size_t max_bin = fft_size / 2; // Use only the positive-frequency half.

	for (size_t i = 0; i <= segment_num; ++i) {
		float fraction = static_cast<float>(i) / segment_num;
		float freq = f_lo * pow(f_hi / f_lo, fraction);        // Logarithmic interpolation.
		auto idx = static_cast<size_t>(freq / delta_f);
		if (idx > max_bin - 1) idx = max_bin - 1;
		boundaries[i] = idx;
	}
	// Remove duplicates so that every segment contains at least one bin.
	for (size_t i = 1; i < boundaries.size(); ++i)
		if (boundaries[i] <= boundaries[i - 1]) boundaries[i] = boundaries[i - 1] + 1;
	if (boundaries.back() > max_bin) boundaries.back() = max_bin;
	return boundaries;
}

void MusicPlayerLibrary::FFTExecuter::MapFreqToSegments(
	std::vector<float>& fft_result,
	std::vector<float>& segments,
	const std::vector<size_t>& bandBounds)
{
	size_t numSegments = bandBounds.size() - 1;
	segments.resize(numSegments);
	for (size_t i = 0; i < numSegments; ++i) {
		float maxVal = 0.0f;
		for (size_t j = bandBounds[i]; j < bandBounds[i + 1]; ++j) {
			if (j >= fft_result.size()) break;
			if (fft_result[j] > maxVal) maxVal = fft_result[j];
		}
		segments[i] = maxVal;
	}
}

void MusicPlayerLibrary::FFTExecuter::ExecuteAudioFFT()
{
	std::vector<uint8_t> raw_samples;
	double spectrum_end_pts_seconds = 0.0;
	std::uint64_t work_epoch = 0;
	{
		std::lock_guard lock(ring_buffer_mutex);
		// Check whether the buffer contains a complete FFT frame.
		if (spectrum_data_ring_buffer.size() < RingBufferWindowBytes || !ring_buffer_has_unprocessed_data)
			return;

		// drain data
		raw_samples.assign(
			spectrum_data_ring_buffer.begin(),
			spectrum_data_ring_buffer.begin() + RingBufferWindowBytes);
		spectrum_end_pts_seconds = ring_buffer_start_pts_seconds +
			static_cast<double>(FftSize) / StandardAudioSampleRate;
		work_epoch = timeline_epoch.load(std::memory_order_acquire);
		const std::size_t hop_bytes = (std::min)(
			FftHopFrames * BytesPerFrame,
			spectrum_data_ring_buffer.size());
		for (std::size_t index = 0; index < hop_bytes; ++index)
			spectrum_data_ring_buffer.pop_front();
		ring_buffer_start_pts_seconds +=
			static_cast<double>(hop_bytes / BytesPerFrame) /
			StandardAudioSampleRate;
		ring_buffer_has_unprocessed_data =
			spectrum_data_ring_buffer.size() >= RingBufferWindowBytes;
	}

	// windowing
	std::vector<double> windowed;
	ApplyWindow(raw_samples, windowed);
	if (windowed.empty())
		return;

	// FFT
	std::vector<float> fft_result;
	DoFFT(windowed, fft_result, fft_cfg.get());

	if (fft_result.empty())
		return;

	// customizable sample rate, 32 segments
	constexpr size_t segment_num = 32;

	auto boundaries = GenBoundaries(
		StandardAudioSampleRate, FftSize, segment_num);

	std::vector<float> new_spectrum;
	MapFreqToSegments(fft_result, new_spectrum, boundaries);

	for (size_t i = 0; i < new_spectrum.size(); ++i) {
			float& val = new_spectrum[i];
			// transition db
			float db = 20.0f * log10f(val + 1e-6f);
			constexpr float db_min = 10.0f;   // supress noise
			constexpr float db_max = 45.0f;   // full
			val = (db - db_min) / (db_max - db_min);
			if (val < 0.0f) val = 0.0f;
			if (val > 1.0f) val = 1.0f;

			// high freq attenuation
			constexpr size_t high_freq_start = segment_num * 2 / 3;
			if (i >= high_freq_start) {
				float attenuation = 1.0f - 0.4f * static_cast<float>(i - high_freq_start) / (segment_num - high_freq_start);
				val *= attenuation;
			}
	}

	{
		std::lock_guard<std::mutex> lock(spectrum_data_mutex);
		if (work_epoch != timeline_epoch.load(std::memory_order_acquire))
			return;
		spectrum_data = new_spectrum;
		spectrum_timeline.push_back({
			.end_pts_seconds = spectrum_end_pts_seconds,
			.bands = std::move(new_spectrum)
		});
		while (spectrum_timeline.size() > MaximumSpectrumQueueSize)
			spectrum_timeline.pop_front();
	}
}

int MusicPlayerLibrary::FFTExecuter::CopyAudioFFTData(float* destination, int destination_length)
{
	if (destination == nullptr || destination_length <= 0)
		return 0;

	std::lock_guard lock(spectrum_data_mutex);
	if (spectrum_timeline.empty())
		return 0;

	const auto& data = spectrum_timeline.back().bands;
	const int copy_count = std::min(destination_length, static_cast<int>(data.size()));
	if (copy_count > 0)
		std::copy_n(data.data(), copy_count, destination);
	return copy_count;
}

int MusicPlayerLibrary::FFTExecuter::CopyAudioFFTDataAt(
	float* destination,
	const int destination_length,
	const double presentation_pts_seconds)
{
	if (destination == nullptr || destination_length <= 0 ||
		!std::isfinite(presentation_pts_seconds))
	{
		return 0;
	}

	std::lock_guard lock(spectrum_data_mutex);
	const auto target = std::upper_bound(
		spectrum_timeline.begin(), spectrum_timeline.end(),
		presentation_pts_seconds,
		[](const double pts, const TimestampedSpectrum& spectrum)
		{
			return pts < spectrum.end_pts_seconds;
		});
	if (target == spectrum_timeline.begin())
		return 0;

	const auto selected = std::prev(target);
	const int copy_count = std::min(
		destination_length, static_cast<int>(selected->bands.size()));
	if (copy_count > 0)
		std::copy_n(selected->bands.data(), copy_count, destination);

	// Once the presentation cursor has passed a spectrum it can no longer be
	// selected. Retain the selected item as a stable fallback for underruns.
	while (spectrum_timeline.size() > 1 &&
		spectrum_timeline[1].end_pts_seconds <= presentation_pts_seconds)
	{
		spectrum_timeline.pop_front();
	}
	return copy_count;
}

void MusicPlayerLibrary::FFTExecuter::ClearSpectrumOutput()
{
	std::lock_guard lock(spectrum_data_mutex);
	spectrum_data.clear();
	spectrum_timeline.clear();
}

void MusicPlayerLibrary::FFTExecuter::ResetBuffers()
{
	{
		std::lock_guard resample_lock(resample_mutex);
		if (resample_context)
		{
			swr_close(resample_context.get());
			if (const int result = swr_init(resample_context.get()); result < 0)
			{
				NATIVE_TRACE("err: reset FFT resample context failed, reason=%s\n",
					GetFFmpegErrorMessage(result).c_str());
				resample_context.reset();
			}
		}
	}

	{
		std::lock_guard lock(ring_buffer_mutex);
		// Advance the epoch while holding the same lock used to snapshot FFT
		// work. A worker can therefore observe either the old epoch with old
		// samples or the new epoch with an empty ring, never a mixture of both.
		timeline_epoch.fetch_add(1, std::memory_order_acq_rel);
		spectrum_data_ring_buffer.clear();
		ring_buffer_has_unprocessed_data = false;
		ring_buffer_start_pts_seconds = 0.0;
		next_input_pts_seconds = 0.0;
		ring_timeline_initialized = false;
	}

	ClearSpectrumOutput();
}

void MusicPlayerLibrary::FFTExecuter::StartFFTThread()
{
	if (fft_thread_running.exchange(true))
		return;

	fft_worker_thread = std::thread(&FFTExecuter::FFTWorkerLoop, this);
}

void MusicPlayerLibrary::FFTExecuter::StopFFTThread()
{
	if (!fft_thread_running.exchange(false))
		return;

	ring_buffer_cv.notify_all();
	if (fft_worker_thread.joinable())
		fft_worker_thread.join();

	ClearSpectrumOutput();
	{
		std::lock_guard lock(ring_buffer_mutex);
		spectrum_data_ring_buffer.clear();
		ring_buffer_has_unprocessed_data = false;
	}
	fft_in.clear();
	fft_out.clear();
}

void MusicPlayerLibrary::FFTExecuter::FFTWorkerLoop()
{
	while (fft_thread_running)
	{
		std::unique_lock lock(ring_buffer_mutex);
		ring_buffer_cv.wait(lock, [this]()
			{
				return !fft_thread_running ||
					(ring_buffer_has_unprocessed_data &&
						spectrum_data_ring_buffer.size() >= RingBufferWindowBytes);
			});

		if (!fft_thread_running)
			break;

		lock.unlock();
		ExecuteAudioFFT();
	}
}

MusicPlayerLibrary::FFTExecuter::FFTExecuter(const AudioOutputFormat& output_format)
{
	if (output_format.sample_rate <= 0 || output_format.channel_count == 0 ||
		output_format.sample_format == AV_SAMPLE_FMT_NONE ||
		av_sample_fmt_is_planar(output_format.sample_format))
	{
		throw std::invalid_argument("FFTExecuter requires a valid packed audio output format");
	}
	input_sample_rate = output_format.sample_rate;

	AVChannelLayout input_layout{};
	int result;
	if (output_format.channel_mask != 0)
		result = av_channel_layout_from_mask(&input_layout, output_format.channel_mask);
	else
	{
		av_channel_layout_default(&input_layout, output_format.channel_count);
		result = input_layout.nb_channels > 0 ? 0 : AVERROR_INVALIDDATA;
	}
	if (result < 0 || input_layout.nb_channels != output_format.channel_count)
	{
		av_channel_layout_uninit(&input_layout);
		throw std::invalid_argument("FFTExecuter input channel layout does not match channel count");
	}

	AVChannelLayout fft_layout = AV_CHANNEL_LAYOUT_STEREO;
	SwrContext* local_resample_context = nullptr;
	result = swr_alloc_set_opts2(
		&local_resample_context,
		&fft_layout,
		AV_SAMPLE_FMT_S16,
		StandardAudioSampleRate,
		&input_layout,
		output_format.sample_format,
		output_format.sample_rate,
		0,
		nullptr);
	av_channel_layout_uninit(&fft_layout);
	av_channel_layout_uninit(&input_layout);
	UniqueSwrContext local_resample(local_resample_context);
	if (result < 0 || !local_resample)
	{
		throw std::runtime_error(
			"swr_alloc_set_opts2 failed: " + GetFFmpegErrorMessage(result));
	}
	result = swr_init(local_resample.get());
	if (result < 0)
	{
		throw std::runtime_error("swr_init failed: " + GetFFmpegErrorMessage(result));
	}

	fft_cfg = AllocateKissFftConfig(static_cast<int>(FftSize));
	if (!fft_cfg)
	{
		throw std::runtime_error("kiss_fft_alloc failed!");
	}
	resample_context = std::move(local_resample);

	fft_in.resize(FftSize);
	fft_out.resize(FftSize);
	StartFFTThread();
}

MusicPlayerLibrary::FFTExecuter::~FFTExecuter()
{
	StopFFTThread();
}
