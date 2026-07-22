// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "Audio/AudioOutputFormat.h"
#include "Audio/FFmpeg/FFmpegResource.h"
#include "Audio/FFT/KissFftConfig.h"

namespace MusicPlayerLibrary
{
	struct AudioFrameData {
		std::vector<uint8_t> data;
		int samples;
	};
	
	// for fast-fourier transform execution
	// 当你觉得自己没用的时候，你可以想想某个瞎改了一晚上把频谱图改成馒头然后回滚的我
	class FFTExecuter
	{
	protected:
		static constexpr size_t BytesPerFrame = 4; // S16 * Stereo
		static constexpr size_t FftSize = 2'048;
		static constexpr size_t RingBufferWindowBytes =
			FftSize * BytesPerFrame;
		static constexpr int FftFrameIntervalMilliseconds = 16;
		static constexpr size_t FftHopFrames =
			StandardAudioSampleRate * FftFrameIntervalMilliseconds / 1'000;
		static constexpr size_t MaximumSpectrumQueueSize = 128;
		static constexpr size_t RingBufferCapacityBytes =
			(FftSize + FftHopFrames * MaximumSpectrumQueueSize) *
			BytesPerFrame;

	public:
		void AddSamplesToRingBuffer(
			const uint8_t* interleaved_samples,
			int frame_count,
			double pts_seconds);
		[[nodiscard]] std::vector<uint8_t> ResampleToFftFormat(
			const uint8_t* interleaved_samples,
			int frame_count);
		[[nodiscard]] int GetRingBufferSize() const;
	protected:
		// apply window to ring buffer, convert to vector
		void ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output);
		void DoFFT(const std::vector<double>& windowed_data, std::vector<float>&, kiss_fft_cfg fft_cfg);
		std::vector<size_t> GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo = 20.0f, float f_hi = 20000.0f);
		void MapFreqToSegments(std::vector<float>&, std::vector<float>&, const std::vector<size_t>&);

	public:
		void ExecuteAudioFFT();
		int CopyAudioFFTData(float* destination, int destination_length);
		int CopyAudioFFTDataAt(
			float* destination,
			int destination_length,
			double presentation_pts_seconds);
		void ResetBuffers();
		void StartFFTThread();
		void StopFFTThread();
		void FFTWorkerLoop();

		explicit FFTExecuter(const AudioOutputFormat& output_format);
		~FFTExecuter();

	protected:
		void ClearSpectrumOutput();

		// ring buffer
		std::deque<uint8_t> spectrum_data_ring_buffer;
		std::vector<float> spectrum_data;
		mutable std::mutex ring_buffer_mutex;
		mutable std::mutex spectrum_data_mutex;
		std::condition_variable ring_buffer_cv;
		bool ring_buffer_has_unprocessed_data = false;
		std::thread fft_worker_thread;
		std::atomic<bool> fft_thread_running{ false };
		std::atomic<std::uint64_t> timeline_epoch{0};

		struct TimestampedSpectrum
		{
			double end_pts_seconds = 0.0;
			std::vector<float> bands;
		};
		// Spectra use the normalized media timeline. Queue depth is deliberately
		// not part of synchronization because Source, FIFO and Sink are decoupled.
		std::deque<TimestampedSpectrum> spectrum_timeline;
		double ring_buffer_start_pts_seconds = 0.0;
		double next_input_pts_seconds = 0.0;
		bool ring_timeline_initialized = false;
		int input_sample_rate = 0;

		// avoid duplicate allocation
		UniqueKissFftConfig fft_cfg;
		std::vector<kiss_fft_cpx> fft_in;
		std::vector<kiss_fft_cpx> fft_out;

		
		// fixed FFT format: StandardAudioSampleRate/S16/Stereo
		UniqueSwrContext resample_context;
		mutable std::mutex resample_mutex;
	};

}
