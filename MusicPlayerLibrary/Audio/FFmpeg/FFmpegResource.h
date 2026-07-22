// SPDX-License-Identifier: MIT

#pragma once

#include <cerrno>
#include <cstdint>
#include <memory>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavfilter/avfilter.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#if defined(__cplusplus)
}
#endif

namespace MusicPlayerLibrary
{
	struct SwrContextDeleter final
	{
		void operator()(SwrContext* context) const noexcept
		{
			if (context != nullptr)
				swr_free(&context);
		}
	};

	using UniqueSwrContext = std::unique_ptr<SwrContext, SwrContextDeleter>;

	struct AvFilterGraphDeleter final
	{
		void operator()(AVFilterGraph* graph) const noexcept
		{
			if (graph != nullptr)
				avfilter_graph_free(&graph);
		}
	};

	using UniqueAvFilterGraph =
		std::unique_ptr<AVFilterGraph, AvFilterGraphDeleter>;

	struct AvAudioFifoDeleter final
	{
		void operator()(AVAudioFifo* fifo) const noexcept
		{
			if (fifo != nullptr)
				av_audio_fifo_free(fifo);
		}
	};

	using UniqueAvAudioFifo = std::unique_ptr<AVAudioFifo, AvAudioFifoDeleter>;

	struct AvFrameDeleter final
	{
		void operator()(AVFrame* frame) const noexcept
		{
			if (frame != nullptr)
				av_frame_free(&frame);
		}
	};

	using UniqueAvFrame = std::unique_ptr<AVFrame, AvFrameDeleter>;

	class AvSamplesBuffer final
	{
		std::uint8_t** data_ = nullptr;

	public:
		AvSamplesBuffer() = default;
		~AvSamplesBuffer() { Reset(); }

		AvSamplesBuffer(const AvSamplesBuffer&) = delete;
		AvSamplesBuffer& operator=(const AvSamplesBuffer&) = delete;

		[[nodiscard]] int Allocate(
			const int channels,
			const int sample_count,
			const AVSampleFormat sample_format) noexcept
		{
			Reset();
			data_ = static_cast<std::uint8_t**>(
				av_calloc(channels, sizeof(std::uint8_t*)));
			if (data_ == nullptr)
				return AVERROR(ENOMEM);

			const int result = av_samples_alloc(
				data_, nullptr, channels, sample_count, sample_format, 0);
			if (result < 0)
				Reset();
			return result;
		}

		[[nodiscard]] std::uint8_t** Get() const noexcept { return data_; }
		[[nodiscard]] std::uint8_t* FirstPlane() const noexcept
		{
			return data_ != nullptr ? data_[0] : nullptr;
		}

		void Reset() noexcept
		{
			if (data_ == nullptr)
				return;
			av_freep(&data_[0]);
			av_free(data_);
			data_ = nullptr;
		}
	};
}
