#define DOCTEST_CONFIG_USE_STD_HEADERS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define NATIVE_REQUIRE(condition, ...) \
    DOCTEST_REQUIRE_MESSAGE(static_cast<bool>(condition), __VA_ARGS__)

#include "Audio/AudioOutputFormat.h"
#include "Audio/AudioOutputCapabilities.h"
#include "Audio/Pipeline/AudioPipeline.h"
#include "Audio/DSP/EqualizerDsp.h"
#include "Audio/DSP/PcmSampleConversion.h"
#include "Audio/FFmpeg/FFmpegResource.h"
#include "Audio/Pipeline/Sink/Common/FapoEqualizer.h"
#include "Audio/Pipeline/Sink/Common/FAudioSink.h"
#include "Audio/Pipeline/Observer/FFTAudioObserve.h"
#include "Audio/Pipeline/Windows/WasapiAudioHelpers.h"
#include "Audio/FFT/AudioPipelinePerformanceHelper.h"
#include "Audio/FFT/FFTExecuter.h"
#include "Lyric/LrcTextParser.h"

#include <FAPOBase.h>

extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
    using namespace MusicPlayerLibrary::AudioDsp;
    using MusicPlayerLibrary::AudioOutputFormat;
    using MusicPlayerLibrary::AudioSinkState;
    using MusicPlayerLibrary::AudioStreamGeneration;
    using MusicPlayerLibrary::DecodedAudioFormat;
    using MusicPlayerLibrary::FAudioSink;
	using MusicPlayerLibrary::FFTAudioObserve;
    using MusicPlayerLibrary::IAudioObserve;
    using MusicPlayerLibrary::IAudioSink;
    using MusicPlayerLibrary::IAudioSource;
    using MusicPlayerLibrary::NormalizedPcmBlock;
    using MusicPlayerLibrary::UniqueAvAudioFifo;
    using MusicPlayerLibrary::UniqueAvFilterGraph;
    using MusicPlayerLibrary::UniqueSwrContext;
    using MusicPlayerLibrary::GetAudioPipelineBufferingProfile;
    using MusicPlayerLibrary::SelectAudioPipelineBufferingProfile;

    struct CapturedPcmBlock
    {
        std::vector<std::uint8_t> bytes;
        std::uint32_t frame_count = 0;
        double pts_seconds = 0.0;
		std::uint64_t stream_frame_offset = 0;
        AudioStreamGeneration generation = 0;
        bool end_of_stream = false;
    };

    CapturedPcmBlock CapturePcmBlock(const NormalizedPcmBlock& block)
    {
        return {
            .bytes = std::vector<std::uint8_t>(
                block.bytes.begin(), block.bytes.end()),
            .frame_count = block.frame_count,
            .pts_seconds = block.pts_seconds,
			.stream_frame_offset = block.stream_frame_offset,
            .generation = block.generation,
            .end_of_stream = block.end_of_stream
        };
    }

    struct FakeOutputDevice
    {
        std::uint64_t identity = 0;
    };

    class FakeAudioSink final : public IAudioSink
    {
        std::shared_ptr<FakeOutputDevice> device_;
        AudioOutputFormat format_{};
        AudioSinkState state_{};
        AudioStreamGeneration next_generation_ = 0;
        AudioStreamGeneration eos_generation_ = 0;
        bool initialized_ = true;
        bool started_ = false;
        float master_volume_ = 1.0f;
        std::array<int, EqualizerBandCount> equalizer_bands_{};
        std::vector<CapturedPcmBlock> submitted_;
        std::vector<CapturedPcmBlock> post_processed_;

        void CapturePostProcessed(const NormalizedPcmBlock& block)
        {
            auto processed = CapturePcmBlock(block);
            // Model sink-owned post-processing without touching the source PCM.
            for (auto& sample_byte : processed.bytes)
                sample_byte ^= std::uint8_t{0x5A};
            post_processed_.push_back(std::move(processed));
        }

    public:
        FakeAudioSink(
            AudioOutputFormat format,
            std::shared_ptr<FakeOutputDevice> device) :
            device_(std::move(device)),
            format_(std::move(format))
        {
        }

        [[nodiscard]] const AudioOutputFormat& GetOutputFormat() const noexcept override
        {
            return format_;
        }

        [[nodiscard]] const AudioOutputFormat& GetDeviceFormat() const noexcept override
        {
            return format_;
        }

        [[nodiscard]] bool IsInitialized() const noexcept override
        {
            return initialized_;
        }

        AudioStreamGeneration BeginStream() override
        {
            state_ = {};
            state_.generation = ++next_generation_;
            eos_generation_ = 0;
            started_ = false;
            return state_.generation;
        }

        bool Submit(const NormalizedPcmBlock& block) override
        {
            if (!initialized_ || state_.generation == 0 ||
                block.generation != state_.generation || block.bytes.empty() ||
                block.frame_count == 0 || state_.stream_ended ||
                eos_generation_ != 0)
            {
                return false;
            }

            submitted_.push_back(CapturePcmBlock(block));
            CapturePostProcessed(block);
            ++state_.buffers_queued;
            state_.queued_frames += block.frame_count;
            if (block.end_of_stream)
                eos_generation_ = block.generation;
            return true;
        }

        bool EndStream() noexcept override
        {
            if (!initialized_ || state_.generation == 0 || state_.stream_ended ||
                eos_generation_ != 0)
                return false;

            const auto current_generation = state_.generation;
            const std::size_t block_align =
                format_.wave_format.Format.nBlockAlign;
            CapturedPcmBlock marker{
                .bytes = std::vector<std::uint8_t>(block_align, std::uint8_t{0}),
                .frame_count = 1,
                .generation = current_generation,
                .end_of_stream = true
            };
            submitted_.push_back(marker);
            for (auto& sample_byte : marker.bytes)
                sample_byte ^= std::uint8_t{0x5A};
            post_processed_.push_back(std::move(marker));
            ++state_.buffers_queued;
            ++state_.queued_frames;
            eos_generation_ = current_generation;
            return true;
        }

        bool Start() noexcept override
        {
            started_ = initialized_ && state_.generation != 0;
            return started_;
        }

        void Stop() noexcept override
        {
            started_ = false;
        }

        void AbortStream() noexcept override
        {
            started_ = false;
            eos_generation_ = 0;
            state_.buffers_queued = 0;
            state_.queued_frames = 0;
            state_.samples_played = 0;
			state_.media_frames_presented = 0;
			state_.presentation_latency_frames = 0;
            state_.stream_ended = false;
        }

        [[nodiscard]] AudioSinkState GetState() const noexcept override
        {
            return state_;
        }

        bool WaitForStreamEnd(std::chrono::milliseconds) override
        {
            return state_.stream_ended;
        }

        void SetMasterVolume(const float volume) noexcept override
        {
            master_volume_ = std::clamp(volume, 0.0f, 1.0f);
        }

        [[nodiscard]] int GetEqualizerBand(const int index) const noexcept override
        {
            return index >= 0 && static_cast<std::size_t>(index) < equalizer_bands_.size()
                ? equalizer_bands_[static_cast<std::size_t>(index)]
                : 0;
        }

        void SetEqualizerBand(const int index, const int value) noexcept override
        {
            if (index >= 0 && static_cast<std::size_t>(index) < equalizer_bands_.size())
            {
                equalizer_bands_[static_cast<std::size_t>(index)] =
                    std::clamp(value, -24, 24);
            }
        }

		[[nodiscard]] bool IsExclusiveModeEnabled() noexcept override
		{
			return false;
		}

        bool SignalStreamEnd(const AudioStreamGeneration generation) noexcept
        {
            if (generation != state_.generation || generation != eos_generation_)
                return false;
            state_.buffers_queued = 0;
            state_.queued_frames = 0;
            state_.samples_played = 0;
			state_.media_frames_presented = 0;
			state_.presentation_latency_frames = 0;
            state_.stream_ended = true;
            started_ = false;
            return true;
        }

        [[nodiscard]] const std::vector<CapturedPcmBlock>& Submitted() const noexcept
        {
            return submitted_;
        }

        [[nodiscard]] const std::vector<CapturedPcmBlock>& PostProcessed() const noexcept
        {
            return post_processed_;
        }

        [[nodiscard]] const std::shared_ptr<FakeOutputDevice>& Device() const noexcept
        {
            return device_;
        }
    };

    class RecordingAudioObserve final : public IAudioObserve
    {
    public:
        std::vector<AudioStreamGeneration> format_generations;
        std::vector<AudioStreamGeneration> reset_generations;
        std::vector<AudioStreamGeneration> eos_generations;
        std::vector<CapturedPcmBlock> pcm_blocks;

        void OnFormat(
            const AudioOutputFormat&,
            const AudioStreamGeneration generation) override
        {
            format_generations.push_back(generation);
        }

        void OnPcm(const NormalizedPcmBlock& block) override
        {
            pcm_blocks.push_back(CapturePcmBlock(block));
        }

        void OnReset(const AudioStreamGeneration generation) override
        {
            reset_generations.push_back(generation);
        }

        void OnEndOfStream(const AudioStreamGeneration generation) override
        {
            eos_generations.push_back(generation);
        }

	};

    class ContractAudioSource final : public IAudioSource
    {
        AudioOutputFormat format_{};
        std::shared_ptr<IAudioSink> sink_;
        std::vector<std::shared_ptr<IAudioObserve>> observers_;
        AudioStreamGeneration generation_ = 0;
		std::uint64_t stream_frame_cursor_ = 0;
        bool eos_observed_ = false;

    public:
        explicit ContractAudioSource(AudioOutputFormat format) :
            format_(std::move(format))
        {
        }

        void Connect(std::shared_ptr<IAudioSink> sink) override
        {
            sink_ = std::move(sink);
        }

        void Subscribe(std::shared_ptr<IAudioObserve> observe) override
        {
            if (observe && std::find(observers_.begin(), observers_.end(), observe) ==
                observers_.end())
            {
                observers_.push_back(std::move(observe));
            }
        }

        void ClearObservers() override
        {
            observers_.clear();
        }

        [[nodiscard]] const AudioOutputFormat& GetNormalizedFormat() const noexcept override
        {
            return format_;
        }

        AudioStreamGeneration BeginNormalizedStream()
        {
            if (!sink_ || !sink_->IsInitialized())
                return 0;
            generation_ = sink_->BeginStream();
			stream_frame_cursor_ = 0;
            eos_observed_ = false;
            for (const auto& observer : observers_)
            {
                observer->OnFormat(format_, generation_);
                observer->OnReset(generation_);
            }
            return generation_;
        }

        bool EmitNormalized(
            const std::span<const std::uint8_t> bytes,
            const std::uint32_t frame_count,
            const double pts_seconds,
            const bool final_block)
        {
            if (!sink_ || generation_ == 0 || eos_observed_)
                return false;

            const NormalizedPcmBlock observed_block{
                .bytes = bytes,
                .frame_count = frame_count,
                .pts_seconds = pts_seconds,
				.stream_frame_offset = stream_frame_cursor_,
                .generation = generation_,
                .end_of_stream = false
            };
            for (const auto& observer : observers_)
                observer->OnPcm(observed_block);

            if (!sink_->Submit(observed_block))
                return false;
			stream_frame_cursor_ += frame_count;

            if (final_block)
            {
                if (!sink_->EndStream())
                    return false;
                eos_observed_ = true;
                for (const auto& observer : observers_)
                    observer->OnEndOfStream(generation_);
            }
            return true;
        }

        [[nodiscard]] AudioStreamGeneration Generation() const noexcept
        {
            return generation_;
        }
    };

    void RequireClose(float expected, float actual, float tolerance,
                      std::string_view message)
    {
        NATIVE_REQUIRE(
            std::isfinite(actual) && std::abs(expected - actual) <= tolerance,
            message);
    }

    UniqueFapo MakeFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter)
    {
        FAPO* effect = nullptr;
        NATIVE_REQUIRE(CreateEqualizerFapo(initial, limiter, &effect) == FAUDIO_OK,
                "CreateEqualizerFapo failed");
        NATIVE_REQUIRE(effect != nullptr, "CreateEqualizerFapo returned a null effect");
        return UniqueFapo(effect);
    }

    FAudioWaveFormatExtensible MakeFloatFormat(
        std::uint32_t sampleRate = 48000,
        std::uint16_t channelCount = 2)
    {
        FAudioWaveFormatExtensible format{};
        format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
        format.Format.nChannels = channelCount;
        format.Format.nSamplesPerSec = sampleRate;
        format.Format.nBlockAlign = static_cast<std::uint16_t>(
            channelCount * sizeof(float));
        format.Format.nAvgBytesPerSec =
            sampleRate * format.Format.nBlockAlign;
        format.Format.wBitsPerSample = 32;
		format.Format.cbSize = MusicPlayerLibrary::FAudioExtensibleFormatExtraSize;
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = channelCount == 2
            ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
            : 0;
		format.SubFormat = MusicPlayerLibrary::IeeeFloatAudioSubFormat;
        return format;
    }

    FAudioWaveFormatExtensible MakePcm24Format(
        std::uint32_t sampleRate = 48000,
        std::uint16_t channelCount = 2,
        std::uint16_t containerBits = 32)
    {
        FAudioWaveFormatExtensible format{};
        format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
        format.Format.nChannels = channelCount;
        format.Format.nSamplesPerSec = sampleRate;
        format.Format.nBlockAlign = static_cast<std::uint16_t>(
            channelCount * containerBits / 8);
        format.Format.nAvgBytesPerSec =
            sampleRate * format.Format.nBlockAlign;
        format.Format.wBitsPerSample = containerBits;
		format.Format.cbSize = MusicPlayerLibrary::FAudioExtensibleFormatExtraSize;
        format.Samples.wValidBitsPerSample = 24;
        format.dwChannelMask = channelCount == 2 ? SPEAKER_STEREO : 0;
		format.SubFormat = MusicPlayerLibrary::PcmAudioSubFormat;
        return format;
    }

    void RequirePackedFrameLayoutMatchesFaudio(
        const MusicPlayerLibrary::AudioOutputFormat& format,
        int frameCount)
    {
        const int bytesPerSample = av_get_bytes_per_sample(format.sample_format);
        NATIVE_REQUIRE(bytesPerSample > 0 && !av_sample_fmt_is_planar(format.sample_format),
                "resolved output must use a valid packed FFmpeg sample format");
        NATIVE_REQUIRE(format.wave_format.Format.nBlockAlign ==
                    format.channel_count * bytesPerSample,
                "FFmpeg and FAudio disagree on the output frame size");
        NATIVE_REQUIRE(format.wave_format.Format.nAvgBytesPerSec ==
                    format.sample_rate * format.wave_format.Format.nBlockAlign,
                "FAudio average byte rate does not match its frame size");

        int lineSize = 0;
        const int fifoBufferBytes = av_samples_get_buffer_size(
            &lineSize, format.channel_count, frameCount,
            format.sample_format, 1);
        const int faudioBufferBytes =
            frameCount * format.wave_format.Format.nBlockAlign;
        NATIVE_REQUIRE(fifoBufferBytes == faudioBufferBytes &&
                    lineSize == faudioBufferBytes,
                "FFmpeg FIFO buffer bytes do not match FAudio frame bytes");
    }

    MusicPlayerLibrary::AudioOutputFormat MakeResolvedOutputFormat(
        MusicPlayerLibrary::AudioChannelMode channelMode,
        MusicPlayerLibrary::AudioBitDepth bitDepth,
        int sampleRate = 48000)
    {
        MusicPlayerLibrary::AudioOutputFormat requested{};
        requested.requested_sample_rate = sampleRate;
        requested.requested_channel_mode = channelMode;
        requested.requested_bit_depth = bitDepth;
        return MusicPlayerLibrary::ResolveAudioOutputFormat(
            requested, MakeFloatFormat(sampleRate));
    }

    std::vector<std::uint8_t> ReferenceSwrFftConversion(
        const MusicPlayerLibrary::AudioOutputFormat& format,
        const std::uint8_t* input,
        int frameCount)
    {
        AVChannelLayout inputLayout{};
        if (format.channel_mask != 0)
            NATIVE_REQUIRE(av_channel_layout_from_mask(&inputLayout, format.channel_mask) >= 0,
                    "reference swresample input layout failed");
        else
            av_channel_layout_default(&inputLayout, format.channel_count);
        NATIVE_REQUIRE(inputLayout.nb_channels == format.channel_count,
                "reference swresample channel count mismatch");

        AVChannelLayout fftLayout = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* rawContext = nullptr;
        const int allocationResult = swr_alloc_set_opts2(
            &rawContext,
            &fftLayout,
            AV_SAMPLE_FMT_S16,
            48000,
            &inputLayout,
            format.sample_format,
            format.sample_rate,
            0,
            nullptr);
        av_channel_layout_uninit(&fftLayout);
        av_channel_layout_uninit(&inputLayout);
        NATIVE_REQUIRE(allocationResult >= 0 && rawContext != nullptr,
                "reference swr_alloc_set_opts2 failed");
        UniqueSwrContext context(rawContext);
        NATIVE_REQUIRE(swr_init(context.get()) >= 0, "reference swr_init failed");

        const int outputCapacity = swr_get_out_samples(context.get(), frameCount);
        NATIVE_REQUIRE(outputCapacity >= 0, "reference swr_get_out_samples failed");
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(outputCapacity) * 2 * sizeof(std::int16_t));
        const std::uint8_t* inputPlanes[] = {input};
        std::uint8_t* outputPlanes[] = {output.data()};
        const int converted = swr_convert(
            context.get(), outputPlanes, outputCapacity,
            inputPlanes, frameCount);
        NATIVE_REQUIRE(converted >= 0, "reference swr_convert failed");
        output.resize(static_cast<std::size_t>(converted) * 2 * sizeof(std::int16_t));
        return output;
    }

    class FFTExecuterProbe final : public MusicPlayerLibrary::FFTExecuter
    {
    public:
        explicit FFTExecuterProbe(const MusicPlayerLibrary::AudioOutputFormat& format)
            : FFTExecuter(format)
        {
            StopFFTThread();
            fft_in.resize(FftSize);
            fft_out.resize(FftSize);
        }

        [[nodiscard]] std::size_t FrameCount() const noexcept
        {
            return FftSize;
        }

        [[nodiscard]] std::size_t FrameByteCount() const noexcept
        {
            return FftSize * BytesPerFrame;
        }

        [[nodiscard]] int SampleRate() const noexcept
        {
            return MusicPlayerLibrary::StandardAudioSampleRate;
        }

        [[nodiscard]] std::vector<std::size_t> Boundaries()
        {
            return GenBoundaries(
                static_cast<float>(MusicPlayerLibrary::StandardAudioSampleRate), FftSize,
                static_cast<std::size_t>(32), 20.0f, 20000.0f);
        }

        [[nodiscard]] std::vector<double> Window(
            const std::vector<std::uint8_t>& samples)
        {
            std::vector<double> windowed;
            ApplyWindow(samples, windowed);
            return windowed;
        }

        [[nodiscard]] std::vector<float> Analyze(
            const std::vector<std::uint8_t>& samples)
        {
            NATIVE_REQUIRE(samples.size() == FrameByteCount(),
                    "FFT probe input must contain exactly one analysis frame");
            {
                std::lock_guard lock(ring_buffer_mutex);
                spectrum_data_ring_buffer.assign(samples.begin(), samples.end());
                ring_buffer_has_unprocessed_data = true;
            }
            ExecuteAudioFFT();

            std::vector<float> result(32);
            const int copied = CopyAudioFFTData(
                result.data(), static_cast<int>(result.size()));
            NATIVE_REQUIRE(copied == static_cast<int>(result.size()),
                    "FFT probe did not return all legacy spectrum bands");
            return result;
        }

		void AddTimelineSamples(
			const std::uint8_t* samples,
			const int frameCount,
			const double streamPositionSeconds)
		{
			AddSamplesToRingBuffer(samples, frameCount, streamPositionSeconds);
			while (GetRingBufferSize() >= static_cast<int>(FrameByteCount()))
				ExecuteAudioFFT();
		}
    };

    std::uint32_t LockFapo(
        FAPO* effect,
        const FAudioWaveFormatExtensible& inputFormat,
        const FAudioWaveFormatExtensible& outputFormat,
        std::uint32_t maxFrameCount = 1024,
        std::uint32_t inputCount = 1,
        std::uint32_t outputCount = 1,
        std::uint32_t outputMaxFrameCount = 0)
    {
        const FAPOLockForProcessBufferParameters input{
            &inputFormat.Format, maxFrameCount};
        const FAPOLockForProcessBufferParameters output{
            &outputFormat.Format,
            outputMaxFrameCount == 0 ? maxFrameCount : outputMaxFrameCount};
        return effect->LockForProcess(
            effect, inputCount, &input, outputCount, &output);
    }

    FAPOProcessBufferParameters ProcessFapo(
        FAPO* effect,
        float* input,
        FAPOBufferFlags inputFlags,
        float* output,
        std::uint32_t frameCount,
        bool enabled)
    {
        const FAPOProcessBufferParameters inputParameters{
            input, inputFlags, frameCount};
        FAPOProcessBufferParameters outputParameters{
            output, FAPO_BUFFER_SILENT, 0};
        effect->Process(
            effect, 1, &inputParameters, 1, &outputParameters, enabled ? 1 : 0);
        return outputParameters;
    }

    std::vector<float> StereoSine(
        std::uint32_t rate, float frequency, float amplitude, std::uint32_t frames)
    {
        std::vector<float> result(frames * 2);
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            const float sample = amplitude * std::sin(
                2.0f * std::numbers::pi_v<float> * frequency * frame / rate);
            result[frame * 2] = result[frame * 2 + 1] = sample;
        }
        return result;
    }

    EqualizerDsp MakePreparedDsp(std::uint32_t sampleRate, std::uint32_t frameCount)
    {
        EqualizerDsp dsp;
        LimiterConfig limiter;
        limiter.enabled = false;
        NATIVE_REQUIRE(dsp.Prepare(sampleRate, 2, frameCount, limiter),
                "EqualizerDsp::Prepare rejected a supported stereo format");
        return dsp;
    }

    EqualizerDsp MakePreparedLimiter(
        std::uint32_t sampleRate,
        std::uint32_t frameCount,
        std::uint32_t channelCount = 2)
    {
        EqualizerDsp dsp;
        LimiterConfig limiter;
        limiter.ceiling = 0.70f;
        NATIVE_REQUIRE(dsp.Prepare(sampleRate, channelCount, frameCount, limiter),
                "EqualizerDsp::Prepare rejected a supported limiter format");
        return dsp;
    }

    void TestZeroDbIdentity()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = 2048;

        auto input = StereoSine(SampleRate, 777.0f, 0.35f, FrameCount);
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
            input[frame * 2 + 1] += 0.05f * std::cos(0.17f * frame);

        std::vector<float> output(input.size());
        auto dsp = MakePreparedDsp(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 1);

        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "EqualizerDsp::Process rejected valid zero-gain input");
        for (std::size_t index = 0; index < input.size(); ++index)
            RequireClose(input[index], output[index], 1.0e-6f,
                         "zero-gain equalizer changed a sample");
    }

    void TestOneKilohertzPlusSixDb()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t WarmUpFrames = 4096;
        constexpr std::uint32_t MeasuredFrames = 4096;
        constexpr std::uint32_t FrameCount = WarmUpFrames + MeasuredFrames;

        auto config = MakeDefaultTenBandConfig();
        config.bands[5].frequency_hz = 1000.0f;
        config.bands[5].q = 1.0f;
        config.bands[5].gain_db = 6.0f;

        const auto input = StereoSine(SampleRate, 1000.0f, 0.1f, FrameCount);
        std::vector<float> output(input.size());
        auto dsp = MakePreparedDsp(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 2);

        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "EqualizerDsp::Process rejected valid boosted input");

        double inputSquareSum = 0.0;
        double outputSquareSum = 0.0;
        for (std::uint32_t frame = WarmUpFrames; frame < FrameCount; ++frame)
        {
            const double inputSample = input[frame * 2];
            const double outputSample = output[frame * 2];
            inputSquareSum += inputSample * inputSample;
            outputSquareSum += outputSample * outputSample;
        }

        const double inputRms = std::sqrt(inputSquareSum / MeasuredFrames);
        const double outputRms = std::sqrt(outputSquareSum / MeasuredFrames);
        const float gainDb = static_cast<float>(20.0 * std::log10(outputRms / inputRms));
        RequireClose(6.0f, gainDb, 0.1f,
                     "1 kHz peaking filter did not produce the configured gain");
    }

    void TestAdaptiveLimiterMatchesInputLoudness()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = SampleRate;
        constexpr std::uint32_t MeasurementStart = SampleRate / 2;
		constexpr std::uint32_t ChunkFrames = 256;

		for (const float equalizerGainDb : {6.0f, -6.0f, 24.0f, -24.0f})
        {
            auto config = MakeDefaultTenBandConfig();
            config.bands[5].frequency_hz = 1000.0f;
            config.bands[5].q = 1.0f;
            config.bands[5].gain_db = equalizerGainDb;
            const auto snapshot = CompileEqualizerSnapshot(
                config, SampleRate, equalizerGainDb > 0.0f ? 31 : 32);

            LimiterConfig limiter;
            limiter.enabled = true;
            limiter.ceiling = 1.0f;
            limiter.match_input_loudness = true;
            EqualizerDsp dsp;
			NATIVE_REQUIRE(dsp.Prepare(SampleRate, 2, ChunkFrames, limiter),
                    "adaptive limiter rejected a supported stereo format");

            const auto input = StereoSine(
                SampleRate, 1000.0f, 0.15f, FrameCount);
            std::vector<float> output(input.size());
			for (std::uint32_t offset = 0; offset < FrameCount;
				offset += ChunkFrames)
			{
				const auto frames = std::min(ChunkFrames, FrameCount - offset);
				NATIVE_REQUIRE(dsp.Process(
						snapshot,
						input.data() + static_cast<std::size_t>(offset) * 2,
						output.data() + static_cast<std::size_t>(offset) * 2,
						frames,
						false),
					"adaptive limiter rejected chunked equalized input");
			}

            double inputSquareSum = 0.0;
            double outputSquareSum = 0.0;
            float outputPeak = 0.0f;
			float wholeStreamOutputPeak = 0.0f;
			for (const float sample : output)
				wholeStreamOutputPeak = std::max(
					wholeStreamOutputPeak, std::abs(sample));
            const auto limiterDelay = dsp.GetLimiterDelayFrames();
            for (std::uint32_t frame = MeasurementStart;
                 frame < FrameCount; ++frame)
            {
                const auto outputIndex = static_cast<std::size_t>(frame) * 2;
                const auto inputFrame = frame >= limiterDelay
                    ? frame - limiterDelay
                    : 0;
                const auto inputIndex = static_cast<std::size_t>(inputFrame) * 2;
                inputSquareSum += input[inputIndex] * input[inputIndex];
                outputSquareSum += output[outputIndex] * output[outputIndex];
                outputPeak = std::max(outputPeak, std::abs(output[outputIndex]));
            }

            const auto measuredFrames = FrameCount - MeasurementStart;
            const double inputRms = std::sqrt(inputSquareSum / measuredFrames);
            const double outputRms = std::sqrt(outputSquareSum / measuredFrames);
            const double loudnessErrorDb = 20.0 * std::log10(outputRms / inputRms);
            NATIVE_REQUIRE(std::isfinite(loudnessErrorDb) &&
						std::abs(loudnessErrorDb) <= 0.5,
                    "adaptive limiter did not match output RMS to decoded input RMS");
            NATIVE_REQUIRE(outputPeak <= 1.0f + 1.0e-6f,
                    "adaptive limiter exceeded full scale");
			if (equalizerGainDb == 24.0f)
			{
				NATIVE_REQUIRE(wholeStreamOutputPeak >= 0.99f,
					"high-level EQ boost did not exercise the peak limiter");
			}
        }
    }

	void TestAdaptiveLimiterResetClearsLoudnessHistory()
	{
		constexpr std::uint32_t SampleRate = 48000;
		constexpr std::uint32_t ChunkFrames = 256;
		LimiterConfig limiter;
		limiter.enabled = true;
		limiter.ceiling = 1.0f;
		limiter.match_input_loudness = true;

		EqualizerDsp reused;
		EqualizerDsp fresh;
		NATIVE_REQUIRE(
			reused.Prepare(SampleRate, 2, ChunkFrames, limiter) &&
			fresh.Prepare(SampleRate, 2, ChunkFrames, limiter),
			"adaptive limiter reset test could not prepare its DSP instances");

		auto boostedConfig = MakeDefaultTenBandConfig();
		boostedConfig.bands[5].gain_db = 24.0f;
		const auto boostedSnapshot = CompileEqualizerSnapshot(
			boostedConfig, SampleRate, 91);
		const auto warmInput = StereoSine(
			SampleRate, 1000.0f, 0.15f, SampleRate / 4);
		std::vector<float> warmOutput(warmInput.size());
		for (std::uint32_t offset = 0; offset < SampleRate / 4;
			offset += ChunkFrames)
		{
			const auto frames = std::min(
				ChunkFrames, SampleRate / 4 - offset);
			NATIVE_REQUIRE(reused.Process(
					boostedSnapshot,
					warmInput.data() + static_cast<std::size_t>(offset) * 2,
					warmOutput.data() + static_cast<std::size_t>(offset) * 2,
					frames,
					false),
				"adaptive limiter warm-up failed");
		}

		auto cutConfig = MakeDefaultTenBandConfig();
		cutConfig.bands[5].gain_db = -24.0f;
		const auto resetSnapshot = CompileEqualizerSnapshot(
			cutConfig, SampleRate, 92);
		const auto resetInput = StereoSine(
			SampleRate, 1000.0f, 0.15f, ChunkFrames);
		std::vector<float> reusedOutput(resetInput.size());
		std::vector<float> freshOutput(resetInput.size());
		NATIVE_REQUIRE(
			reused.Process(resetSnapshot, resetInput.data(), reusedOutput.data(),
				ChunkFrames, false) &&
			fresh.Process(resetSnapshot, resetInput.data(), freshOutput.data(),
				ChunkFrames, false),
			"adaptive limiter reset generation was rejected");
		for (std::size_t index = 0; index < reusedOutput.size(); ++index)
		{
			RequireClose(
				freshOutput[index], reusedOutput[index], 1.0e-6f,
				"reset generation retained adaptive loudness history");
		}
	}

	void TestAdaptiveLimiterMatchesStackedEqualizerBands()
	{
		constexpr std::uint32_t SampleRate = 48000;
		constexpr std::uint32_t FrameCount = SampleRate;
		constexpr std::uint32_t ChunkFrames = 256;
		constexpr std::uint32_t MeasurementStart = SampleRate / 2;

		for (const float bandGainDb : {24.0f, -24.0f})
		{
			auto config = MakeDefaultTenBandConfig();
			for (auto& band : config.bands)
				band.gain_db = bandGainDb;
			const auto snapshot = CompileEqualizerSnapshot(
				config, SampleRate, bandGainDb > 0.0f ? 93 : 94);

			LimiterConfig limiter;
			limiter.enabled = true;
			limiter.ceiling = 1.0f;
			limiter.match_input_loudness = true;
			EqualizerDsp dsp;
			NATIVE_REQUIRE(dsp.Prepare(
					SampleRate, 2, ChunkFrames, limiter),
				"stacked-EQ adaptive limiter could not be prepared");

			const auto input = StereoSine(
				SampleRate, 1000.0f, 0.01f, FrameCount);
			std::vector<float> output(input.size());
			for (std::uint32_t offset = 0; offset < FrameCount;
				offset += ChunkFrames)
			{
				const auto frames = std::min(ChunkFrames, FrameCount - offset);
				NATIVE_REQUIRE(dsp.Process(
						snapshot,
						input.data() + static_cast<std::size_t>(offset) * 2,
						output.data() + static_cast<std::size_t>(offset) * 2,
						frames,
						false),
					"stacked-EQ adaptive limiter rejected a PCM chunk");
			}

			double inputSquareSum = 0.0;
			double outputSquareSum = 0.0;
			float outputPeak = 0.0f;
			const auto limiterDelay = dsp.GetLimiterDelayFrames();
			for (std::uint32_t frame = MeasurementStart;
				frame < FrameCount; ++frame)
			{
				const auto outputIndex = static_cast<std::size_t>(frame) * 2;
				const auto inputIndex = static_cast<std::size_t>(
					frame - limiterDelay) * 2;
				inputSquareSum += input[inputIndex] * input[inputIndex];
				outputSquareSum += output[outputIndex] * output[outputIndex];
				outputPeak = std::max(outputPeak, std::abs(output[outputIndex]));
			}
			const double loudnessErrorDb = 10.0 * std::log10(
				outputSquareSum / inputSquareSum);
			NATIVE_REQUIRE(std::isfinite(loudnessErrorDb) &&
					std::abs(loudnessErrorDb) <= 0.75,
				std::format(
					"adaptive limiter could not compensate stacked legal EQ bands: gain={} dB, error={} dB",
					bandGainDb,
					loudnessErrorDb));
			NATIVE_REQUIRE(outputPeak <= 1.0f + 1.0e-6f,
				"stacked-EQ adaptive limiter exceeded full scale");
		}
	}

    void TestNyquistAndConfiguredRates()
    {
        constexpr std::uint32_t FrameCount = 1024;

        auto config = MakeDefaultTenBandConfig();
        for (auto& band : config.bands)
        {
            band.q = 1.0f;
            band.gain_db = 3.0f;
        }

        for (const auto sampleRate : MusicPlayerLibrary::AudioOutputProbeSampleRates)
        {
            const auto snapshot = CompileEqualizerSnapshot(config, sampleRate, 3);
            for (const auto& coefficients : snapshot.bands)
            {
                NATIVE_REQUIRE(std::isfinite(coefficients.b0) &&
                        std::isfinite(coefficients.b1) &&
                        std::isfinite(coefficients.b2) &&
                        std::isfinite(coefficients.a1) &&
                        std::isfinite(coefficients.a2),
                        "configured-rate coefficients must remain finite");
            }

            if (sampleRate == 16000)
            {
                const auto& coefficients = snapshot.bands[9];
                NATIVE_REQUIRE(coefficients.b0 == 1.0f && coefficients.b1 == 0.0f &&
                        coefficients.b2 == 0.0f && coefficients.a1 == 0.0f &&
                        coefficients.a2 == 0.0f,
                        "16 kHz band must be identity at a 16 kHz sample rate");
                NATIVE_REQUIRE((snapshot.enabled_mask & (1u << 9)) == 0,
                        "Nyquist-rejected 16 kHz band must not be enabled");
            }

            const auto input = StereoSine(sampleRate, 1000.0f, 0.1f, FrameCount);
            std::vector<float> output(input.size());
            auto dsp = MakePreparedDsp(sampleRate, FrameCount);
            NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                    "EqualizerDsp::Process rejected a configured sample rate");
            NATIVE_REQUIRE(std::all_of(output.begin(), output.end(),
                                [](float sample) { return std::isfinite(sample); }),
                    "configured-rate output must remain finite");
        }
    }

    void TestLimiterDelayAtConfiguredRates()
    {
        struct DelayCase
        {
            std::uint32_t sample_rate;
            std::uint32_t delay_frames;
        };
        constexpr std::array<DelayCase, 2> Cases{{
            {48000, 239},
            {44100, 219}
        }};

        for (const auto& testCase : Cases)
        {
            const std::uint32_t frameCount = testCase.delay_frames + 2;
            std::vector<float> input(static_cast<std::size_t>(frameCount) * 2, 0.0f);
            std::vector<float> output(input.size(), -1.0f);
            input[0] = input[1] = 0.5f;

            auto dsp = MakePreparedLimiter(testCase.sample_rate, frameCount);
            const auto snapshot = CompileEqualizerSnapshot(
                MakeDefaultTenBandConfig(), testCase.sample_rate, 10);

            NATIVE_REQUIRE(dsp.GetLimiterDelayFrames() == testCase.delay_frames,
                    "limiter reported the wrong look-ahead delay");
            NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), frameCount, false),
                    "limiter rejected an impulse input");
            for (std::uint32_t frame = 0; frame < testCase.delay_frames; ++frame)
            {
                RequireClose(0.0f, output[static_cast<std::size_t>(frame) * 2],
                             1.0e-7f, "limiter impulse appeared before its delay");
                RequireClose(0.0f, output[static_cast<std::size_t>(frame) * 2 + 1],
                             1.0e-7f, "limiter impulse appeared before its delay");
            }
            RequireClose(0.5f,
                         output[static_cast<std::size_t>(testCase.delay_frames) * 2],
                         1.0e-6f, "limiter impulse appeared at the wrong frame");
            RequireClose(0.5f,
                         output[static_cast<std::size_t>(testCase.delay_frames) * 2 + 1],
                         1.0e-6f, "limiter impulse appeared at the wrong frame");
        }
    }

    void TestLimiterLinksStereoAtCeiling()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t DelayFrames = 239;
        constexpr std::uint32_t FrameCount = DelayFrames + 1;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2, 0.0f);
        std::vector<float> output(input.size(), 0.0f);
        input[0] = 1.0f;
        input[1] = 0.25f;

        auto dsp = MakePreparedLimiter(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 11);
        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "limiter rejected linked-stereo input");

        RequireClose(0.70f, output[static_cast<std::size_t>(DelayFrames) * 2],
                     1.0e-6f, "limiter did not enforce its ceiling");
        RequireClose(0.175f, output[static_cast<std::size_t>(DelayFrames) * 2 + 1],
                     1.0e-6f, "limiter did not apply linked gain to stereo");
    }

    void TestLimiterDoesNotApplyMakeupGain()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t DelayFrames = 239;
        constexpr std::uint32_t MeasuredFrames = 32;
        constexpr std::uint32_t FrameCount = DelayFrames + MeasuredFrames;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2, 0.5f);
        std::vector<float> output(input.size(), 0.0f);

        auto dsp = MakePreparedLimiter(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 12);
        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "limiter rejected below-ceiling input");

        for (std::uint32_t frame = DelayFrames; frame < FrameCount; ++frame)
        {
            RequireClose(0.5f, output[static_cast<std::size_t>(frame) * 2],
                         1.0e-6f, "limiter applied make-up gain");
            RequireClose(0.5f, output[static_cast<std::size_t>(frame) * 2 + 1],
                         1.0e-6f, "limiter applied make-up gain");
        }
    }

    void TestLimiterReleaseIsLinear()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t DelayFrames = 239;
        constexpr std::uint32_t ReleaseFrames = 2400;
        constexpr std::uint32_t FrameCount = DelayFrames + ReleaseFrames + 1;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2, 0.5f);
        std::vector<float> output(input.size(), 0.0f);
        input[0] = input[1] = 1.0f;

        auto dsp = MakePreparedLimiter(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 13);
        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "limiter rejected release-ramp input");

        const auto leftAt = [&output](std::uint32_t frame)
        {
            return output[static_cast<std::size_t>(frame) * 2];
        };
        const float firstReleaseGain = leftAt(DelayFrames + 1) / 0.5f;
        const float midpointGain = leftAt(DelayFrames + ReleaseFrames / 2) / 0.5f;
        const float finalGain = leftAt(DelayFrames + ReleaseFrames) / 0.5f;
        RequireClose(0.70f, leftAt(DelayFrames), 1.0e-6f,
                     "limiter peak did not begin at ceiling gain");
        RequireClose(0.70f + 0.30f / ReleaseFrames, firstReleaseGain, 2.0e-6f,
                     "limiter first release increment was not linear");
        RequireClose(0.85f, midpointGain, 2.0e-5f,
                     "limiter release midpoint was not linear");
        RequireClose(1.0f, finalGain, 2.0e-5f,
                     "limiter did not finish release in 50 ms");
    }

    void TestLimiterSilentInputDrainsTail()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t DelayFrames = 239;
        auto dsp = MakePreparedLimiter(SampleRate, DelayFrames);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 14);
        const std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{};

        NATIVE_REQUIRE(dsp.Process(snapshot, impulse.data(), initialOutput.data(), 1, false),
                "limiter rejected queued tail input");
        NATIVE_REQUIRE(dsp.HasTail(), "limiter did not report a queued tail");

        std::vector<float> drained(static_cast<std::size_t>(DelayFrames) * 2, -1.0f);
        NATIVE_REQUIRE(dsp.Process(snapshot, nullptr, drained.data(), DelayFrames, true),
                "silent processing did not return the queued impulse");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2],
                     1.0e-6f, "silent processing did not drain the queued impulse");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2 + 1],
                     1.0e-6f, "silent processing did not drain the queued impulse");

        std::array<float, 2> finalOutput{1.0f, 1.0f};
        NATIVE_REQUIRE(!dsp.Process(snapshot, nullptr, finalOutput.data(), 1, true),
                "silent processing reported a tail after it drained");
        RequireClose(0.0f, finalOutput[0], 1.0e-7f,
                     "drained limiter emitted a nonzero sample");
        RequireClose(0.0f, finalOutput[1], 1.0e-7f,
                     "drained limiter emitted a nonzero sample");
        NATIVE_REQUIRE(!dsp.HasTail(), "limiter tail did not clear after silent draining");
    }

    void TestLimiterResetGenerationDropsTail()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t DelayFrames = 239;
        auto dsp = MakePreparedLimiter(SampleRate, DelayFrames + 1);
        const auto firstSnapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 15);
        const auto resetSnapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 16);
        const std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{};
        NATIVE_REQUIRE(dsp.Process(firstSnapshot, impulse.data(), initialOutput.data(), 1, false),
                "limiter rejected pre-reset input");
        NATIVE_REQUIRE(dsp.HasTail(), "limiter did not queue pre-reset history");

        std::vector<float> output(static_cast<std::size_t>(DelayFrames + 1) * 2, 1.0f);
        NATIVE_REQUIRE(!dsp.Process(resetSnapshot, nullptr, output.data(), DelayFrames + 1, true),
                "reset limiter reported discarded history as a tail");
        NATIVE_REQUIRE(std::all_of(output.begin(), output.end(),
                            [](float sample) { return sample == 0.0f; }),
                "reset generation did not discard queued limiter history");
        NATIVE_REQUIRE(!dsp.HasTail(), "reset generation left limiter tail state behind");
    }

    void TestLimiterIsChunkInvariant()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = 4093;
        constexpr std::uint32_t ChunkFrames = 127;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2);
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
        {
            const float carrier = 0.58f * std::sin(0.071f * frame);
            input[static_cast<std::size_t>(frame) * 2] =
                frame % 503 == 0 ? 1.2f : carrier;
            input[static_cast<std::size_t>(frame) * 2 + 1] =
                frame % 337 == 0 ? -0.95f : 0.37f * std::cos(0.043f * frame);
        }

        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 3.0f;
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 17);
        std::vector<float> oneBlock(input.size(), 0.0f);
        auto wholeDsp = MakePreparedLimiter(SampleRate, FrameCount);
        NATIVE_REQUIRE(wholeDsp.Process(snapshot, input.data(), oneBlock.data(), FrameCount, false),
                "limiter rejected one-block input");

        std::vector<float> chunked = input;
        auto chunkedDsp = MakePreparedLimiter(SampleRate, FrameCount);
        for (std::uint32_t offset = 0; offset < FrameCount; offset += ChunkFrames)
        {
            const std::uint32_t count = std::min(ChunkFrames, FrameCount - offset);
            float* const block = chunked.data() + static_cast<std::size_t>(offset) * 2;
            NATIVE_REQUIRE(chunkedDsp.Process(snapshot, block, block, count, false),
                    "limiter rejected a 127-frame in-place chunk");
        }

        for (std::size_t index = 0; index < oneBlock.size(); ++index)
        {
            RequireClose(oneBlock[index], chunked[index], 1.0e-6f,
                         "limiter output changed with chunk size");
        }
    }

    void TestFapoFactoryAndRegistration()
    {
        constexpr std::uint32_t SampleRate = 48000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 20);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto effect = MakeFapo(snapshot, limiter);

        NATIVE_REQUIRE(effect->AddRef(effect.get()) == 2,
                "factory reference count was not one");
        NATIVE_REQUIRE(effect->Release(effect.get()) == 1,
                "factory reference count did not return to one");

        FAPORegistrationProperties* properties = nullptr;
        NATIVE_REQUIRE(effect->GetRegistrationProperties(
                    effect.get(), &properties) == FAUDIO_OK &&
                properties != nullptr,
                "FAPO registration properties were unavailable");
        constexpr std::uint32_t RequiredFlags =
            FAPO_FLAG_CHANNELS_MUST_MATCH |
            FAPO_FLAG_FRAMERATE_MUST_MATCH |
            FAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
            FAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
            FAPO_FLAG_INPLACE_SUPPORTED;
        NATIVE_REQUIRE(properties->Flags == RequiredFlags,
                "FAPO registration flags were incorrect");
        NATIVE_REQUIRE((properties->Flags & FAPO_FLAG_INPLACE_REQUIRED) == 0,
                "FAPO incorrectly required in-place processing");
        reinterpret_cast<FAPOBase*>(effect.get())->pFree(properties);
    }

    void TestFapoAdvertisesExtensibleFormats()
    {
        constexpr std::uint32_t SampleRate = 48000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 29);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto valid = MakeFloatFormat(SampleRate);

        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), &valid.Format, &valid.Format, nullptr) ==
                    FAUDIO_OK,
                "FAPO did not advertise its supported extensible input format");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), &valid.Format, &valid.Format, nullptr) ==
                    FAUDIO_OK,
                "FAPO did not advertise its supported extensible output format");

        auto legacyFloat = valid;
        legacyFloat.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
        legacyFloat.Format.cbSize = 0;
        FAudioWaveFormatEx* closestFormat = nullptr;
        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), &valid.Format, &legacyFloat.Format,
                    &closestFormat) == FAPO_E_FORMAT_UNSUPPORTED &&
                closestFormat == nullptr,
                "FAPO advertised an unsupported legacy input format");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), &valid.Format, &legacyFloat.Format,
                    &closestFormat) == FAPO_E_FORMAT_UNSUPPORTED &&
                closestFormat == nullptr,
                "FAPO advertised an unsupported legacy output format");

        auto mismatched = valid;
        mismatched.dwChannelMask = 0;
        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), &valid.Format, &mismatched.Format, nullptr) ==
                    FAPO_E_FORMAT_UNSUPPORTED,
                "FAPO advertised a nonmatching input format pair");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), &valid.Format, &mismatched.Format, nullptr) ==
                    FAPO_E_FORMAT_UNSUPPORTED,
                "FAPO advertised a nonmatching output format pair");

        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), nullptr, &valid.Format, nullptr) ==
                    FAUDIO_E_INVALID_ARG,
                "input-format probing did not safely reject a null output format");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), nullptr, &valid.Format, nullptr) ==
                    FAUDIO_E_INVALID_ARG,
                "output-format probing did not safely reject a null input format");
    }

    void TestFapoInPlaceMatchesOutOfPlace()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = 2048;
        const auto initial = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 21);
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 6.0f;
        const auto updated = CompileEqualizerSnapshot(config, SampleRate, 22);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto inPlaceEffect = MakeFapo(initial, limiter);
        auto outOfPlaceEffect = MakeFapo(initial, limiter);
        inPlaceEffect->SetParameters(
            inPlaceEffect.get(), &updated, sizeof(updated));
        outOfPlaceEffect->SetParameters(
            outOfPlaceEffect.get(), &updated, sizeof(updated));

        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(inPlaceEffect.get(), format, format, FrameCount) == FAUDIO_OK,
                "in-place FAPO lock failed");
        NATIVE_REQUIRE(LockFapo(outOfPlaceEffect.get(), format, format, FrameCount) == FAUDIO_OK,
                "out-of-place FAPO lock failed");

        auto input = StereoSine(SampleRate, 1000.0f, 0.1f, FrameCount);
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
            input[static_cast<std::size_t>(frame) * 2 + 1] +=
                0.03f * std::cos(0.11f * frame);
        auto inPlace = input;
        std::vector<float> outOfPlace(
            input.size(), std::numeric_limits<float>::quiet_NaN());

        const auto inPlaceResult = ProcessFapo(
            inPlaceEffect.get(), inPlace.data(), FAPO_BUFFER_VALID,
            inPlace.data(), FrameCount, true);
        const auto outOfPlaceResult = ProcessFapo(
            outOfPlaceEffect.get(), input.data(), FAPO_BUFFER_VALID,
            outOfPlace.data(), FrameCount, true);
        NATIVE_REQUIRE(inPlaceResult.BufferFlags == FAPO_BUFFER_VALID &&
                outOfPlaceResult.BufferFlags == FAPO_BUFFER_VALID &&
                inPlaceResult.ValidFrameCount == FrameCount &&
                outOfPlaceResult.ValidFrameCount == FrameCount,
                "FAPO did not report the complete valid output");
        for (std::size_t index = 0; index < inPlace.size(); ++index)
        {
            RequireClose(inPlace[index], outOfPlace[index], 1.0e-6f,
                         "in-place and out-of-place FAPO output differed");
        }
        NATIVE_REQUIRE(std::any_of(
                    outOfPlace.begin(), outOfPlace.end(),
                    [&input, index = std::size_t{0}](float sample) mutable
                    { return sample != input[index++]; }),
                "full-size parameter update was not applied");

        inPlaceEffect->UnlockForProcess(inPlaceEffect.get());
        outOfPlaceEffect->UnlockForProcess(outOfPlaceEffect.get());
    }

    void TestFapoDisabledValidPassThrough()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = 257;
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 9.0f;
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 23);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                "disabled pass-through FAPO lock failed");

        auto input = StereoSine(SampleRate, 777.0f, 0.37f, FrameCount);
		std::vector<float> warmOutput(input.size(), 0.0f);
		const auto warmResult = ProcessFapo(
			effect.get(), input.data(), FAPO_BUFFER_VALID,
			warmOutput.data(), FrameCount, true);
		NATIVE_REQUIRE(warmResult.ValidFrameCount == FrameCount &&
				MusicPlayerLibrary::AudioDsp::EqualizerFapoHasTail(effect.get()),
			"enabled FAPO did not establish state before its disable transition");

        std::vector<float> output(
            input.size(), std::numeric_limits<float>::quiet_NaN());
        const auto result = ProcessFapo(
            effect.get(), input.data(), FAPO_BUFFER_VALID,
            output.data(), FrameCount, false);
        NATIVE_REQUIRE(result.BufferFlags == FAPO_BUFFER_VALID &&
                result.ValidFrameCount == FrameCount,
                "disabled pass-through did not preserve valid metadata");
        NATIVE_REQUIRE(output == input,
                "disabled out-of-place FAPO was not an exact pass-through");

		std::vector<float> resetOutput(input.size(), 1.0f);
		const auto resetResult = ProcessFapo(
			effect.get(), nullptr, FAPO_BUFFER_SILENT,
			resetOutput.data(), FrameCount, true);
		NATIVE_REQUIRE(resetResult.BufferFlags == FAPO_BUFFER_SILENT &&
				std::all_of(
					resetOutput.begin(), resetOutput.end(),
					[](const float sample) { return sample == 0.0f; }) &&
				!MusicPlayerLibrary::AudioDsp::EqualizerFapoHasTail(effect.get()),
			"re-enabled FAPO replayed DSP state retained across disable");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoRejectsUnsafeProcessBuffers()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t LockedFrameCount = 1;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 30);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto effect = MakeFapo(snapshot, limiter);
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(
                    effect.get(), format, format, LockedFrameCount) == FAUDIO_OK,
                "process-bounds FAPO lock failed");

        std::array<float, 4> oversizedInput{0.1f, 0.2f, 0.3f, 0.4f};
        std::array<float, 4> guardedOutput{-1.0f, -2.0f, 1234.0f, 5678.0f};
        const auto originalGuardedOutput = guardedOutput;
        const auto oversizedResult = ProcessFapo(
            effect.get(), oversizedInput.data(), FAPO_BUFFER_VALID,
            guardedOutput.data(), LockedFrameCount + 1, false);
        NATIVE_REQUIRE(oversizedResult.BufferFlags == FAPO_BUFFER_SILENT &&
                oversizedResult.ValidFrameCount == 0,
                "FAPO did not reject a frame count larger than its lock");
        NATIVE_REQUIRE(guardedOutput == originalGuardedOutput,
                "rejected oversized processing touched output or its sentinel");

        std::array<float, 2> output{11.0f, 12.0f};
        const auto originalOutput = output;
        const auto nullInputResult = ProcessFapo(
            effect.get(), nullptr, FAPO_BUFFER_VALID,
            output.data(), LockedFrameCount, false);
        NATIVE_REQUIRE(nullInputResult.BufferFlags == FAPO_BUFFER_SILENT &&
                nullInputResult.ValidFrameCount == 0 && output == originalOutput,
                "FAPO did not safely reject a null VALID input buffer");

        const auto nullOutputResult = ProcessFapo(
            effect.get(), oversizedInput.data(), FAPO_BUFFER_VALID,
            nullptr, LockedFrameCount, false);
        NATIVE_REQUIRE(nullOutputResult.BufferFlags == FAPO_BUFFER_SILENT &&
                nullOutputResult.ValidFrameCount == 0,
                "FAPO did not safely reject a required null output buffer");

        const auto silentResult = ProcessFapo(
            effect.get(), nullptr, FAPO_BUFFER_SILENT,
            output.data(), LockedFrameCount, false);
        NATIVE_REQUIRE(silentResult.BufferFlags == FAPO_BUFFER_SILENT &&
                silentResult.ValidFrameCount == LockedFrameCount &&
                output[0] == 0.0f && output[1] == 0.0f,
                "FAPO incorrectly required input storage for SILENT input");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoSilentInputNeverReadsBuffer()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = 127;
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 6.0f;
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 24);
        LimiterConfig limiter;
        limiter.enabled = false;
        const auto format = MakeFloatFormat(SampleRate);
        std::vector<float> nanInput(
            static_cast<std::size_t>(FrameCount) * 2,
            std::numeric_limits<float>::quiet_NaN());

        for (const bool enabled : {false, true})
        {
            auto effect = MakeFapo(snapshot, limiter);
            NATIVE_REQUIRE(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                    "silent-input FAPO lock failed");
            std::vector<float> output(
                nanInput.size(), std::numeric_limits<float>::quiet_NaN());
            const auto result = ProcessFapo(
                effect.get(), nanInput.data(), FAPO_BUFFER_SILENT,
                output.data(), FrameCount, enabled);
            NATIVE_REQUIRE(result.BufferFlags == FAPO_BUFFER_SILENT &&
                    result.ValidFrameCount == FrameCount,
                    "drained silent input was not reported silent");
            NATIVE_REQUIRE(std::all_of(
                        output.begin(), output.end(),
                        [](float sample) { return sample == 0.0f; }),
                    "silent NaN input was read or output was not fully written");
            effect->UnlockForProcess(effect.get());
        }
    }

    void TestFapoRejectsInvalidParameterBlocks()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t FrameCount = 1024;
        const auto initial = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 25);
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 12.0f;
        const auto validUpdate = CompileEqualizerSnapshot(config, SampleRate, 26);
        LimiterConfig limiter;
        limiter.enabled = false;
        const auto format = MakeFloatFormat(SampleRate);
        const auto input = StereoSine(SampleRate, 1000.0f, 0.1f, FrameCount);

        const auto verifyRejected = [&](EqualizerDspSnapshot update,
                                        std::uint32_t byteCount,
                                        std::string_view message)
        {
            auto effect = MakeFapo(initial, limiter);
            effect->SetParameters(effect.get(), &update, byteCount);
            NATIVE_REQUIRE(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                    "parameter-validation FAPO lock failed");
            std::vector<float> output(
                input.size(), std::numeric_limits<float>::quiet_NaN());
            const auto result = ProcessFapo(
                effect.get(), const_cast<float*>(input.data()), FAPO_BUFFER_VALID,
                output.data(), FrameCount, true);
            NATIVE_REQUIRE(result.BufferFlags == FAPO_BUFFER_VALID && output == input,
                    message);
            effect->UnlockForProcess(effect.get());
        };

        verifyRejected(
            validUpdate, sizeof(EqualizerDspSnapshot) - 1,
            "short FAPO parameter block changed parameters");
        auto wrongByteSize = validUpdate;
        wrongByteSize.byte_size = sizeof(EqualizerDspSnapshot) - 1;
        verifyRejected(
            wrongByteSize, sizeof(wrongByteSize),
            "FAPO accepted a snapshot with the wrong byte_size");
        auto wrongAbi = validUpdate;
        ++wrongAbi.abi_version;
        verifyRejected(
            wrongAbi, sizeof(wrongAbi),
            "FAPO accepted a snapshot with the wrong ABI version");
    }

    void TestFapoGetParametersValidatesDestination()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint8_t Sentinel = 0xa5;
        const auto initial = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 31);
        auto effect = MakeFapo(initial, LimiterConfig{});

        std::array<
            std::uint8_t,
            sizeof(EqualizerDspSnapshot) + sizeof(std::uint32_t)> oversized{};
        oversized.fill(Sentinel);
        effect->GetParameters(
            effect.get(), oversized.data(),
            static_cast<std::uint32_t>(oversized.size()));
        NATIVE_REQUIRE(std::all_of(
                    oversized.begin(), oversized.end(),
                    [](std::uint8_t value) { return value == Sentinel; }),
                "FAPO GetParameters accepted an oversized destination");

        std::array<std::uint8_t, sizeof(EqualizerDspSnapshot)> undersized{};
        undersized.fill(Sentinel);
        effect->GetParameters(
            effect.get(), undersized.data(),
            sizeof(EqualizerDspSnapshot) - 1);
        NATIVE_REQUIRE(std::all_of(
                    undersized.begin(), undersized.end(),
                    [](std::uint8_t value) { return value == Sentinel; }),
                "FAPO GetParameters accepted an undersized destination");

        EqualizerDspSnapshot actual{};
        effect->GetParameters(effect.get(), &actual, sizeof(actual));
        NATIVE_REQUIRE(std::memcmp(&actual, &initial, sizeof(actual)) == 0,
                "FAPO GetParameters did not return the exact current snapshot");

        effect->GetParameters(
            effect.get(), nullptr, sizeof(EqualizerDspSnapshot));
    }

    void TestFapoLockRejectsInvalidFormats()
    {
        constexpr std::uint32_t SampleRate = 48000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 27);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto valid = MakeFloatFormat(SampleRate);

        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid, 128, 0, 1) != FAUDIO_OK,
                "FAPO accepted zero input buffers");
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid, 128, 1, 2) != FAUDIO_OK,
                "FAPO accepted two output buffers");

        auto mismatchedChannels = valid;
        mismatchedChannels.Format.nChannels = 1;
        mismatchedChannels.Format.nBlockAlign = sizeof(float);
        mismatchedChannels.Format.nAvgBytesPerSec = SampleRate * sizeof(float);
        mismatchedChannels.dwChannelMask = 0;
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, mismatchedChannels) != FAUDIO_OK,
                "FAPO accepted mismatched channel formats");

        auto legacyFloat = valid;
        legacyFloat.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
        legacyFloat.Format.cbSize = 0;
        NATIVE_REQUIRE(LockFapo(effect.get(), legacyFloat, legacyFloat) != FAUDIO_OK,
                "FAPO accepted a non-extensible float format");

        auto pcm = valid;
		pcm.SubFormat = MusicPlayerLibrary::PcmAudioSubFormat;
        NATIVE_REQUIRE(LockFapo(effect.get(), pcm, pcm) != FAUDIO_OK,
                "FAPO accepted a non-float32 extensible format");

        auto mismatchedRate = valid;
        mismatchedRate.Format.nSamplesPerSec = 44100;
        mismatchedRate.Format.nAvgBytesPerSec =
            mismatchedRate.Format.nBlockAlign * 44100;
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, mismatchedRate) != FAUDIO_OK,
                "FAPO accepted mismatched sample rates");

        auto mismatchedMask = valid;
        mismatchedMask.dwChannelMask = 0;
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, mismatchedMask) != FAUDIO_OK,
                "FAPO accepted nonmatching extensible formats");
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid, 128, 1, 1, 256) != FAUDIO_OK,
                "FAPO accepted mismatched maximum frame counts");

        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid) == FAUDIO_OK,
                "FAPO rejected an exact float32 extensible format pair");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoSilentInputDrainsTail()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t DelayFrames = 239;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 28);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(effect.get(), format, format, DelayFrames) == FAUDIO_OK,
                "tail-drain FAPO lock failed");

        std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        const auto initialResult = ProcessFapo(
            effect.get(), impulse.data(), FAPO_BUFFER_VALID,
            initialOutput.data(), 1, true);
        NATIVE_REQUIRE(initialResult.BufferFlags == FAPO_BUFFER_VALID,
                "valid input did not keep FAPO output valid");

        std::vector<float> silentInput(
            static_cast<std::size_t>(DelayFrames) * 2,
            std::numeric_limits<float>::quiet_NaN());
        std::vector<float> drained(
            silentInput.size(), std::numeric_limits<float>::quiet_NaN());
        const auto drainResult = ProcessFapo(
            effect.get(), silentInput.data(), FAPO_BUFFER_SILENT,
            drained.data(), DelayFrames, true);
        NATIVE_REQUIRE(drainResult.BufferFlags == FAPO_BUFFER_VALID,
                "FAPO stopped reporting valid output before its tail ended");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2],
                     1.0e-6f, "FAPO did not drain its limiter tail");

        std::array<float, 2> finalInput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        std::array<float, 2> finalOutput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        const auto finalResult = ProcessFapo(
            effect.get(), finalInput.data(), FAPO_BUFFER_SILENT,
            finalOutput.data(), 1, true);
        NATIVE_REQUIRE(finalResult.BufferFlags == FAPO_BUFFER_SILENT &&
                finalOutput[0] == 0.0f && finalOutput[1] == 0.0f,
                "FAPO did not become silent after its tail ended");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoValidZeroBlocksDrainUntilReportedTailEnds()
    {
        constexpr std::uint32_t SampleRate = 48000;
        constexpr std::uint32_t ChunkFrames = 256;
        constexpr std::uint32_t OldFixedDrainFrames = SampleRate * 55 / 1000;
        constexpr std::uint32_t SafetyLimitFrames = SampleRate * 3;

        auto config = MakeDefaultTenBandConfig();
        // Exercise the slowest production EQ band at its public gain limit.
        // Q remains the shipping default because callers cannot change it.
        config.bands[0].gain_db = 24.0f;
        const auto snapshot = CompileEqualizerSnapshot(
            config, SampleRate, 38);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto effect = MakeFapo(snapshot, limiter);
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(
            LockFapo(effect.get(), format, format, ChunkFrames) == FAUDIO_OK,
            "valid-zero tail-drain FAPO lock failed");

        std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> impulseOutput{};
        const auto initialSequence = EqualizerFapoProcessSequence(effect.get());
        (void)ProcessFapo(
            effect.get(), impulse.data(), FAPO_BUFFER_VALID,
            impulseOutput.data(), 1, true);
        NATIVE_REQUIRE(
            EqualizerFapoProcessSequence(effect.get()) == initialSequence + 1,
            "FAPO process sequence did not publish the impulse pass");
        NATIVE_REQUIRE(EqualizerFapoHasTail(effect.get()),
            "FAPO helper did not publish the equalizer tail");

        std::vector<float> zeros(static_cast<std::size_t>(ChunkFrames) * 2, 0.0f);
        std::vector<float> output(zeros.size());
        std::uint32_t drainedFrames = 0;
        while (EqualizerFapoHasTail(effect.get()) &&
               drainedFrames < SafetyLimitFrames)
        {
            const auto before = EqualizerFapoProcessSequence(effect.get());
            (void)ProcessFapo(
                effect.get(), zeros.data(), FAPO_BUFFER_VALID,
                output.data(), ChunkFrames, true);
            NATIVE_REQUIRE(
                EqualizerFapoProcessSequence(effect.get()) == before + 1,
                "FAPO process sequence did not publish a zero-drain pass");
            drainedFrames += ChunkFrames;
        }

        NATIVE_REQUIRE(!EqualizerFapoHasTail(effect.get()),
            "valid zero blocks did not fully drain the reported DSP tail");
        NATIVE_REQUIRE(drainedFrames > OldFixedDrainFrames,
            "tail regression fixture no longer exceeds the removed 55 ms heuristic");
        effect->UnlockForProcess(effect.get());
    }

    void TestAudioPipelineBufferingProfiles()
    {
        const auto fast = SelectAudioPipelineBufferingProfile(15.0);
        NATIVE_REQUIRE(fast.fifo_target_milliseconds == 80 &&
            fast.decoded_queue_target_milliseconds == 24,
            "fast CPU buffering profile changed unexpectedly");

        const auto balanced = SelectAudioPipelineBufferingProfile(15.01);
        NATIVE_REQUIRE(balanced.fifo_target_milliseconds == 140 &&
            balanced.decoded_queue_target_milliseconds == 32,
            "balanced CPU buffering profile changed unexpectedly");

        const auto slow = SelectAudioPipelineBufferingProfile(50.01);
        NATIVE_REQUIRE(slow.fifo_target_milliseconds == 220 &&
            slow.decoded_queue_target_milliseconds == 48,
            "slow CPU buffering profile changed unexpectedly");

        const auto fallback = SelectAudioPipelineBufferingProfile(
            std::numeric_limits<double>::quiet_NaN());
        NATIVE_REQUIRE(fallback.fifo_target_milliseconds == 140 &&
            fallback.decoded_queue_target_milliseconds == 32,
            "failed FFT benchmark did not select the safe fallback profile");
        NATIVE_REQUIRE(fast.fifo_target_milliseconds > fast.decoded_queue_target_milliseconds &&
            balanced.fifo_target_milliseconds > balanced.decoded_queue_target_milliseconds &&
            slow.fifo_target_milliseconds > slow.decoded_queue_target_milliseconds,
            "buffering profiles must keep most depth in the normalized PCM FIFO");

        const auto& measured = GetAudioPipelineBufferingProfile();
        NATIVE_REQUIRE(measured.fifo_target_milliseconds >
            measured.decoded_queue_target_milliseconds,
            "measured CPU profile did not favor the normalized PCM FIFO");
    }

    void TestAudioOutputFormatMappings()
    {
        const auto systemFormat = MakeFloatFormat(48000, 2);
        struct Mapping
        {
            MusicPlayerLibrary::AudioChannelMode mode;
            std::uint16_t channels;
            std::uint32_t mask;
            std::string_view layout;
        };
        constexpr Mapping mappings[] = {
            {MusicPlayerLibrary::AudioChannelMode::Mono, 1, SPEAKER_MONO, "mono"},
            {MusicPlayerLibrary::AudioChannelMode::Stereo, 2, SPEAKER_STEREO, "stereo"},
            {MusicPlayerLibrary::AudioChannelMode::Surround51, 6, SPEAKER_5POINT1_SURROUND, "5.1(side)"},
            {MusicPlayerLibrary::AudioChannelMode::Surround71, 8, SPEAKER_7POINT1_SURROUND, "7.1"}
        };

        for (const auto& mapping : mappings)
        {
            MusicPlayerLibrary::AudioOutputFormat requested{};
            requested.requested_sample_rate = 96000;
            requested.requested_channel_mode = mapping.mode;
            requested.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit16;
            const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
                requested, systemFormat);
            NATIVE_REQUIRE(resolved.sample_rate == 96000,
                    "explicit output sample rate was not retained");
            NATIVE_REQUIRE(resolved.channel_count == mapping.channels &&
                    resolved.channel_mask == mapping.mask &&
                    resolved.ffmpeg_channel_layout == mapping.layout,
                    "explicit channel mode mapped to the wrong FFmpeg/FAudio layout");
            NATIVE_REQUIRE(resolved.sample_format == AV_SAMPLE_FMT_S16 &&
                    resolved.wave_format.Format.wBitsPerSample == 16 &&
                    resolved.wave_format.Format.nBlockAlign == mapping.channels * 2,
                    "16-bit output format was not resolved consistently");
        }

        MusicPlayerLibrary::AudioOutputFormat floatRequest{};
        floatRequest.requested_channel_mode = MusicPlayerLibrary::AudioChannelMode::Stereo;
        floatRequest.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit32;
        const auto floatResolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            floatRequest, systemFormat);
        NATIVE_REQUIRE(floatResolved.sample_rate == 48000 &&
                floatResolved.sample_format == AV_SAMPLE_FMT_FLT &&
                floatResolved.wave_format.Format.wBitsPerSample == 32 &&
                floatResolved.wave_format.Format.nBlockAlign == 8,
                "32-bit float output format was not resolved consistently");
    }

    void TestSinkPreGainIsAppliedInsideDsp()
    {
        constexpr std::uint32_t SampleRate = 48000;
		constexpr std::uint32_t FrameCount = 4096;
        constexpr float PreGain = 0.70f;

        const auto input = StereoSine(SampleRate, 777.0f, 0.35f, FrameCount);
        std::vector<float> output(input.size());
        auto dsp = MakePreparedDsp(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 2, PreGain);

        NATIVE_REQUIRE(dsp.Process(
                    snapshot, input.data(), output.data(), FrameCount, false),
                "sink pre-gain snapshot was rejected");
        for (std::size_t index = 0; index < input.size(); ++index)
        {
            RequireClose(
                input[index] * PreGain,
                output[index],
                1.0e-6f,
                "sink pre-gain was not applied before the DSP output");
        }
    }

    void TestBitPerfectFormatHandshakeRequiresExactPackedPcm()
    {
        const auto output = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48000);
        const DecodedAudioFormat matching{
            .sample_rate = 48000,
            .channel_count = 2,
            .channel_mask = SPEAKER_STEREO,
            .bit_depth = 16,
            .sample_format = AV_SAMPLE_FMT_S16
        };
        NATIVE_REQUIRE(MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    matching, output),
                "an exact packed decoded/output format did not bypass normalization");

		const auto pcm24Output = MakeResolvedOutputFormat(
			MusicPlayerLibrary::AudioChannelMode::Stereo,
			MusicPlayerLibrary::AudioBitDepth::Bit24,
			48000);
		const DecodedAudioFormat matchingPcm24{
			.sample_rate = 48000,
			.channel_count = 2,
			.channel_mask = SPEAKER_STEREO,
			.bit_depth = 24,
			.sample_format = AV_SAMPLE_FMT_S32
		};
		NATIVE_REQUIRE(MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matchingPcm24, pcm24Output),
			"packed PCM24-in-S32 did not satisfy its exact sink contract");

		const auto floatOutput = MakeResolvedOutputFormat(
			MusicPlayerLibrary::AudioChannelMode::Stereo,
			MusicPlayerLibrary::AudioBitDepth::Bit32,
			48000);
		const DecodedAudioFormat matchingFloat{
			.sample_rate = 48000,
			.channel_count = 2,
			.channel_mask = SPEAKER_STEREO,
			.bit_depth = 32,
			.sample_format = AV_SAMPLE_FMT_FLT
		};
		NATIVE_REQUIRE(MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matchingFloat, floatOutput),
			"packed float32 did not satisfy its exact sink contract");

		AudioOutputFormat legacyPcm16{};
		AudioOutputFormat legacyFloat32{};
		const auto pcm32Output =
			MusicPlayerLibrary::MakeWasapiPcm32Variant(floatOutput);
		AudioOutputFormat legacyPcm32{};
		const DecodedAudioFormat matchingPcm32{
			.sample_rate = 48000,
			.channel_count = 2,
			.channel_mask = SPEAKER_STEREO,
			.bit_depth = 32,
			.sample_format = AV_SAMPLE_FMT_S32
		};
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryMakeWasapiLegacyVariant(output, legacyPcm16) &&
			MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, legacyPcm16),
			"exact legacy PCM16 unnecessarily required normalization");
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryMakeWasapiLegacyVariant(
				floatOutput, legacyFloat32) &&
			MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matchingFloat, legacyFloat32),
			"exact legacy float32 unnecessarily required normalization");
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryMakeWasapiLegacyVariant(
				pcm32Output, legacyPcm32) &&
			MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matchingPcm32, legacyPcm32),
			"exact legacy PCM32 unnecessarily required normalization");
		auto malformedLegacy = legacyPcm16;
		malformedLegacy.wave_format.Format.wFormatTag =
			FAUDIO_FORMAT_IEEE_FLOAT;
		NATIVE_REQUIRE(
			!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedLegacy),
			"a legacy descriptor with the wrong encoding was marked bit-perfect");

        auto mismatch = matching;
        mismatch.sample_rate = 44100;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    mismatch, output),
                "a sample-rate conversion was marked bit-perfect");
        mismatch = matching;
        mismatch.channel_count = 1;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    mismatch, output),
                "a channel-count conversion was marked bit-perfect");
        mismatch = matching;
        mismatch.channel_mask = SPEAKER_MONO;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    mismatch, output),
                "a channel-layout conversion was marked bit-perfect");
        mismatch = matching;
        mismatch.bit_depth = 24;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    mismatch, output),
                "a valid-bit-depth conversion was marked bit-perfect");
        mismatch = matching;
        mismatch.sample_format = AV_SAMPLE_FMT_S16P;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    mismatch, output),
                "planar PCM was marked as a byte-for-byte sink match");
        mismatch = matching;
        mismatch.channel_mask = 0;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    mismatch, output),
                "an unknown source layout was marked bit-perfect");

        auto malformedOutput = output;
        malformedOutput.wave_format.Format.nBlockAlign = 8;
        NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
                    matching, malformedOutput),
                "a mismatched sink frame size was marked bit-perfect");
		malformedOutput = output;
		malformedOutput.wave_format.Format.nSamplesPerSec = 44100;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedOutput),
			"inconsistent sink WAVE sample rate was marked bit-perfect");
		malformedOutput = output;
		malformedOutput.wave_format.Format.nChannels = 1;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedOutput),
			"inconsistent sink WAVE channel count was marked bit-perfect");
		malformedOutput = output;
		malformedOutput.wave_format.dwChannelMask = SPEAKER_MONO;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedOutput),
			"inconsistent sink WAVE channel mask was marked bit-perfect");
		malformedOutput = output;
		malformedOutput.wave_format.SubFormat =
			MusicPlayerLibrary::IeeeFloatAudioSubFormat;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedOutput),
			"inconsistent sink WAVE subformat was marked bit-perfect");
		malformedOutput = output;
		++malformedOutput.wave_format.Format.nAvgBytesPerSec;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedOutput),
			"inconsistent sink WAVE byte rate was marked bit-perfect");
		malformedOutput = output;
		++malformedOutput.wave_format.Format.cbSize;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				matching, malformedOutput),
			"noncanonical WAVE extensible size was marked bit-perfect");
		malformedOutput = output;
		malformedOutput.bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit24;
		malformedOutput.wave_format.Samples.wValidBitsPerSample = 24;
		auto malformedInput = matching;
		malformedInput.bit_depth = 24;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				malformedInput, malformedOutput),
			"valid bits wider than the PCM container were marked bit-perfect");
		malformedOutput = output;
		malformedOutput.channel_mask = SPEAKER_MONO;
		malformedOutput.wave_format.dwChannelMask = SPEAKER_MONO;
		malformedInput = matching;
		malformedInput.channel_mask = SPEAKER_MONO;
		NATIVE_REQUIRE(!MusicPlayerLibrary::AreAudioFormatsBitPerfect(
				malformedInput, malformedOutput),
			"a channel mask with the wrong speaker count was marked bit-perfect");
    }

    void TestRawAudioFormatInfoMappings()
    {
        static_assert(std::is_trivially_copyable_v<
            MusicPlayerLibrary::AudioFormatInfo>);

        constexpr int Unknown = static_cast<int>(
            MusicPlayerLibrary::AudioChannelMode::Unknown);
        const MusicPlayerLibrary::AudioFormatInfo empty{};
        NATIVE_REQUIRE(empty.channel_type_id == Unknown &&
                    empty.sample_rate == 0 &&
                    empty.bit_depth == static_cast<int>(
                        MusicPlayerLibrary::AudioBitDepth::Unknown),
                "raw audio format defaults were not unknown");

        struct Mapping
        {
            MusicPlayerLibrary::AudioChannelMode mode;
            int expectedChannelTypeId;
        };
        constexpr Mapping mappings[] = {
            {MusicPlayerLibrary::AudioChannelMode::Mono, 1},
            {MusicPlayerLibrary::AudioChannelMode::Stereo, 2},
            {MusicPlayerLibrary::AudioChannelMode::Surround51, 3},
            {MusicPlayerLibrary::AudioChannelMode::Surround71, 4}
        };
        for (const auto& mapping : mappings)
        {
            MusicPlayerLibrary::AudioOutputFormat requested{};
            requested.requested_sample_rate = 96000;
            requested.requested_channel_mode = mapping.mode;
            requested.requested_bit_depth =
                MusicPlayerLibrary::AudioBitDepth::Bit24;
            const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
                requested, MakeFloatFormat());
            const auto info = MusicPlayerLibrary::GetAudioFormatInfo(resolved);
            NATIVE_REQUIRE(info.channel_type_id == mapping.expectedChannelTypeId &&
                        info.sample_rate == 96000 && info.bit_depth == 24,
                    "resolved output was not converted to raw format metadata");
        }

        NATIVE_REQUIRE(MusicPlayerLibrary::GetAudioChannelTypeId(
                    6, SPEAKER_5POINT1) == 3 &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(
                    6, SPEAKER_5POINT1_SURROUND) == 3 &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(
                    8, SPEAKER_7POINT1) == 4 &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(
                    8, SPEAKER_7POINT1_SURROUND) == 4,
                "standard surround layouts were assigned the wrong channel type id");
        NATIVE_REQUIRE(MusicPlayerLibrary::GetAudioChannelTypeId(4) == Unknown &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(6) == Unknown &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(8) == Unknown,
                "non-specific channel layouts were misclassified as surround");
    }

    void TestAudioSourceBitrateCalculation()
    {
        const double bitrate =
            MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                32000, 2.0);
        NATIVE_REQUIRE(std::abs(bitrate - 16.0) < 1.0e-9,
                "128 kbit/s did not resolve to 16 KByte/s");
        NATIVE_REQUIRE(MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                    256000, 0.0) == 0.0 &&
                    MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                    0, 2.0) == 0.0,
                "audio bitrate accepted an incomplete observation");
        NATIVE_REQUIRE(MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                    256000, std::numeric_limits<double>::infinity()) == 0.0,
                "audio bitrate accepted a non-finite duration");

        MusicPlayerLibrary::AudioBitrateTracker tracker;
        NATIVE_REQUIRE(tracker.GetKBytesPerSecond() == 0.0,
                "audio bitrate tracker did not start empty");
        tracker.ObserveEncodedBytes(16000);
        tracker.ObserveDecodedSamples(12000, 48000);
        tracker.ObserveDecodedSamples(12000, 48000);
        NATIVE_REQUIRE(std::abs(tracker.GetKBytesPerSecond() - 32.0) < 1.0e-9,
                "audio bitrate tracker did not combine multiple decoded frames");

        tracker.ObserveEncodedBytes(48000);
        tracker.ObserveDecodedSamples(24000, 48000);
        NATIVE_REQUIRE(std::abs(tracker.GetKBytesPerSecond() - 64.0) < 1.0e-9,
                "audio bitrate tracker did not compute a VBR weighted average");

        tracker.ObserveEncodedBytes(0);
        tracker.ObserveDecodedSamples(1024, 0);
        NATIVE_REQUIRE(std::abs(tracker.GetKBytesPerSecond() - 64.0) < 1.0e-9,
                "audio bitrate tracker accepted an invalid observation");
        tracker.Reset();
        NATIVE_REQUIRE(tracker.GetKBytesPerSecond() == 0.0,
                "audio bitrate tracker did not reset for a new decode epoch");
    }

    void TestAverageAudioQualityClassification()
    {
        const double thresholdBitrate =
            MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                15000000, 200.0);
        NATIVE_REQUIRE(std::abs(thresholdBitrate - 600000.0) < 1.0e-9,
                "whole-stream average bitrate was calculated incorrectly");
        NATIVE_REQUIRE(!MusicPlayerLibrary::IsLoselessAudio(thresholdBitrate),
                "exactly 600 kbit/s was classified as loseless audio");

        const double aboveThresholdBitrate =
            MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                25000000, 200.0);
        NATIVE_REQUIRE(MusicPlayerLibrary::IsLoselessAudio(aboveThresholdBitrate),
                "audio above 600 kbit/s was not classified as loseless");
        NATIVE_REQUIRE(!MusicPlayerLibrary::IsLoselessAudio(
                    MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                        25000000, 0.0)),
                "audio with an unknown duration was classified as loseless");
        NATIVE_REQUIRE(MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                    25000000, -1.0) == 0.0 &&
                    MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                    25000000, std::numeric_limits<double>::quiet_NaN()) == 0.0 &&
                    !MusicPlayerLibrary::IsLoselessAudio(
                        std::numeric_limits<double>::infinity()),
                "invalid average bitrate inputs were accepted");

        NATIVE_REQUIRE(!MusicPlayerLibrary::IsHiResAudio(48000) &&
                    MusicPlayerLibrary::IsHiResAudio(48001) &&
                    MusicPlayerLibrary::IsHiResAudio(96000),
                "Hi-Res sample-rate threshold was applied incorrectly");
    }

    void TestExplicit24BitAudioOutputFormatMapping()
    {
        MusicPlayerLibrary::AudioOutputFormat requested{};
        requested.requested_sample_rate = 96000;
        requested.requested_channel_mode =
            MusicPlayerLibrary::AudioChannelMode::Stereo;
        requested.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit24;
        const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            requested, MakeFloatFormat());
        const auto& waveFormat = resolved.wave_format;

        NATIVE_REQUIRE(resolved.sample_rate == 96000 &&
                    resolved.channel_count == 2 &&
                    resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit24,
                "explicit 24-bit output request was not retained");
        NATIVE_REQUIRE(resolved.sample_format == AV_SAMPLE_FMT_S32,
                "explicit 24-bit output did not use FFmpeg's S32 container");
        NATIVE_REQUIRE(waveFormat.Format.wFormatTag == FAUDIO_FORMAT_EXTENSIBLE &&
                    waveFormat.Format.wBitsPerSample == 32 &&
                    waveFormat.Samples.wValidBitsPerSample == 24 &&
                    waveFormat.Format.nBlockAlign == 8 &&
                    MusicPlayerLibrary::GuidEquals(
                        waveFormat.SubFormat,
						MusicPlayerLibrary::PcmAudioSubFormat),
                "explicit 24-bit output did not use 24-valid-in-32 PCM");
        RequirePackedFrameLayoutMatchesFaudio(resolved, 257);
    }

    void TestSystemAudioOutputFormatMapping()
    {
        auto systemFormat = MakeFloatFormat(44100, 6);
        systemFormat.dwChannelMask = SPEAKER_5POINT1_SURROUND;
        const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            MusicPlayerLibrary::AudioOutputFormat{}, systemFormat);
        NATIVE_REQUIRE(resolved.sample_rate == 44100 &&
                resolved.channel_count == 6 &&
                resolved.channel_mask == SPEAKER_5POINT1_SURROUND &&
                resolved.ffmpeg_channel_layout == "5.1(side)",
                "System channel mode did not use the device format");
        NATIVE_REQUIRE(resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit32 &&
                resolved.sample_format == AV_SAMPLE_FMT_FLT,
                "System bit depth did not use the float device format");
        const auto resolvedInfo = MusicPlayerLibrary::GetAudioFormatInfo(resolved);
        NATIVE_REQUIRE(resolvedInfo.channel_type_id == 3 &&
                    resolvedInfo.sample_rate == 44100 &&
                    resolvedInfo.bit_depth == 32,
                "System request did not report the resolved device metadata");

        systemFormat.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
        systemFormat.Format.wBitsPerSample = 16;
        systemFormat.Samples.wValidBitsPerSample = 16;
		systemFormat.SubFormat = MusicPlayerLibrary::PcmAudioSubFormat;
        const auto pcmResolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            MusicPlayerLibrary::AudioOutputFormat{}, systemFormat);
        NATIVE_REQUIRE(pcmResolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit16 &&
                pcmResolved.sample_format == AV_SAMPLE_FMT_S16,
                "System bit depth did not use the PCM16 device format");
        NATIVE_REQUIRE(MusicPlayerLibrary::GetAudioFormatInfo(pcmResolved).bit_depth == 16,
                "PCM16 device metadata reported the wrong bit depth");
    }

    void TestSystem24BitPcmOutputFormatMapping()
    {
        constexpr std::uint16_t SystemContainerBits[] = {24, 32};
        for (const auto containerBits : SystemContainerBits)
        {
            const auto systemFormat = MakePcm24Format(
                88200, 2, containerBits);
            const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
                MusicPlayerLibrary::AudioOutputFormat{}, systemFormat);
            const auto& waveFormat = resolved.wave_format;

            NATIVE_REQUIRE(resolved.sample_rate == 88200 &&
                        resolved.channel_count == 2 &&
                        resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit24,
                    "System PCM24 format was not detected from its valid bits");
            NATIVE_REQUIRE(resolved.sample_format == AV_SAMPLE_FMT_S32,
                    "System PCM24 format did not resolve to FFmpeg S32");
            NATIVE_REQUIRE(waveFormat.Format.wFormatTag == FAUDIO_FORMAT_EXTENSIBLE &&
                        waveFormat.Format.wBitsPerSample == 32 &&
                        waveFormat.Samples.wValidBitsPerSample == 24 &&
                        waveFormat.Format.nBlockAlign == 8 &&
                        MusicPlayerLibrary::GuidEquals(
                            waveFormat.SubFormat,
							MusicPlayerLibrary::PcmAudioSubFormat),
                    "System PCM24 format did not normalize to 24-valid-in-32 PCM");
            RequirePackedFrameLayoutMatchesFaudio(resolved, 511);
        }
    }

    void TestAresampleAcceptsOutputFormats()
    {
        const auto systemFormat = MakeFloatFormat(48000, 2);
        for (const auto channelMode : MusicPlayerLibrary::AudioOutputProbeChannelModes)
        {
            for (const auto bitDepth : MusicPlayerLibrary::AudioOutputProbeBitDepths)
            {
                MusicPlayerLibrary::AudioOutputFormat requested{};
                requested.requested_sample_rate = 48000;
                requested.requested_channel_mode = channelMode;
                requested.requested_bit_depth = bitDepth;
                const auto output = MusicPlayerLibrary::ResolveAudioOutputFormat(
                    requested, systemFormat);
                const char* sampleFormatName = av_get_sample_fmt_name(output.sample_format);
                NATIVE_REQUIRE(sampleFormatName != nullptr,
                        "resolved output format has no FFmpeg sample-format name");

                UniqueAvFilterGraph graph(avfilter_graph_alloc());
                NATIVE_REQUIRE(graph != nullptr, "avfilter_graph_alloc failed");
                AVFilterContext* source = nullptr;
                AVFilterContext* resample = nullptr;
                AVFilterContext* format = nullptr;
                AVFilterContext* sink = nullptr;
                const std::string sourceArgs =
                    "sample_rate=44100:sample_fmt=fltp:channel_layout=stereo";
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("abuffer"), "src",
                    sourceArgs.c_str(), nullptr, graph.get()) >= 0,
                    "FFmpeg rejected the test abuffer format");
                const std::string resampleArgs = std::format(
                    "sample_rate={}:out_chlayout={}:out_sample_fmt={}",
                    output.sample_rate, output.ffmpeg_channel_layout, sampleFormatName);
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &resample, avfilter_get_by_name("aresample"), "resample",
                    resampleArgs.c_str(), nullptr, graph.get()) >= 0,
                    "aresample rejected a resolved output format");
                const std::string formatArgs = std::format(
                    "sample_fmts={}:sample_rates={}:channel_layouts={}",
                    sampleFormatName, output.sample_rate, output.ffmpeg_channel_layout);
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &format, avfilter_get_by_name("aformat"), "format",
                    formatArgs.c_str(), nullptr, graph.get()) >= 0,
                    "aformat rejected a resolved output format");
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &sink, avfilter_get_by_name("abuffersink"), "sink",
                    nullptr, nullptr, graph.get()) >= 0,
                    "avfilter_graph_create_filter failed for abuffersink");
                NATIVE_REQUIRE(avfilter_link(source, 0, resample, 0) >= 0 &&
                        avfilter_link(resample, 0, format, 0) >= 0 &&
                        avfilter_link(format, 0, sink, 0) >= 0,
                        "failed to link the aresample test graph");
                NATIVE_REQUIRE(avfilter_graph_config(graph.get(), nullptr) >= 0,
                        "FFmpeg could not configure a resolved output format");
                const AVFilterLink* outputLink = sink->inputs[0];
                NATIVE_REQUIRE(outputLink->sample_rate == output.sample_rate &&
                        outputLink->ch_layout.nb_channels == output.channel_count &&
                        outputLink->format == output.sample_format,
                        "aresample graph negotiated a different output format");
            }
        }
    }

    void TestFftResamplesToLegacyFormat()
    {
        const std::array<std::int16_t, 2> mono{16384, -16384};
        const auto monoFormat = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Mono,
            MusicPlayerLibrary::AudioBitDepth::Bit16);
        MusicPlayerLibrary::FFTExecuter monoExecutor(monoFormat);
        const auto monoResult = monoExecutor.ResampleToFftFormat(
            reinterpret_cast<const std::uint8_t*>(mono.data()), 2);
        const auto monoReference = ReferenceSwrFftConversion(
            monoFormat, reinterpret_cast<const std::uint8_t*>(mono.data()), 2);
        NATIVE_REQUIRE(monoResult == monoReference,
                "mono FFT conversion did not produce reference S16 stereo PCM");
        NATIVE_REQUIRE(monoResult.size() == 2 * 2 * sizeof(std::int16_t),
                "mono FFT conversion returned the wrong byte count");
        std::array<std::int16_t, 4> monoPcm{};
        std::memcpy(monoPcm.data(), monoResult.data(), monoResult.size());
        NATIVE_REQUIRE(monoPcm[0] == monoPcm[1] && monoPcm[0] > 0 &&
                    monoPcm[2] == monoPcm[3] && monoPcm[2] < 0,
                "mono FFT conversion did not produce matching stereo channels");

        constexpr int InputFrames = 1024;
        std::vector<float> stereo(static_cast<std::size_t>(InputFrames) * 2);
        for (int frame = 0; frame < InputFrames; ++frame)
        {
            stereo[static_cast<std::size_t>(frame) * 2] =
                0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f *
                                static_cast<float>(frame) / 44100.0f);
            stereo[static_cast<std::size_t>(frame) * 2 + 1] =
                0.25f * std::cos(2.0f * std::numbers::pi_v<float> * 880.0f *
                                 static_cast<float>(frame) / 44100.0f);
        }
        const auto stereoFormat = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit32,
            44100);
        MusicPlayerLibrary::FFTExecuter stereoExecutor(stereoFormat);
        const auto stereoResult = stereoExecutor.ResampleToFftFormat(
            reinterpret_cast<const std::uint8_t*>(stereo.data()), InputFrames);
        const auto stereoReference = ReferenceSwrFftConversion(
            stereoFormat, reinterpret_cast<const std::uint8_t*>(stereo.data()),
            InputFrames);
        NATIVE_REQUIRE(stereoResult == stereoReference,
                "44.1 kHz float FFT conversion differed from 48 kHz S16 stereo reference");
        NATIVE_REQUIRE(!stereoResult.empty() &&
                    stereoResult.size() % (2 * sizeof(std::int16_t)) == 0,
                "FFT conversion did not return packed S16 stereo frames");
    }

    void Test24BitFifoAndFftFrameSizing()
    {
        constexpr int FrameCount = 4;
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit24,
            48000);
        RequirePackedFrameLayoutMatchesFaudio(format, FrameCount);

        const std::array<std::int32_t, FrameCount * 2> input{
            0x40000000, -0x40000000,
            0x20000000, -0x20000000,
            0x10000000, -0x10000000,
            0x08000000, -0x08000000};
        NATIVE_REQUIRE(sizeof(input) == static_cast<std::size_t>(
                    FrameCount * format.wave_format.Format.nBlockAlign),
                "PCM24 test storage does not contain whole FAudio frames");

        UniqueAvAudioFifo fifo(av_audio_fifo_alloc(
            format.sample_format, format.channel_count, FrameCount));
        NATIVE_REQUIRE(fifo != nullptr, "failed to allocate the PCM24 audio FIFO");
        void* writePlanes[] = {const_cast<std::int32_t*>(input.data())};
        NATIVE_REQUIRE(av_audio_fifo_write(fifo.get(), writePlanes, FrameCount) == FrameCount &&
                    av_audio_fifo_size(fifo.get()) == FrameCount,
                "PCM24 FIFO did not preserve the input frame count");

        std::array<std::int32_t, FrameCount * 2> fifoOutput{};
        void* readPlanes[] = {fifoOutput.data()};
        NATIVE_REQUIRE(av_audio_fifo_read(fifo.get(), readPlanes, FrameCount) == FrameCount &&
                    fifoOutput == input,
                "PCM24 FIFO round-trip changed S32 container samples");

        MusicPlayerLibrary::FFTExecuter executor(format);
        const auto* fifoBytes = reinterpret_cast<const std::uint8_t*>(
            fifoOutput.data());
        const auto fftPcm = executor.ResampleToFftFormat(fifoBytes, FrameCount);
        const auto reference = ReferenceSwrFftConversion(
            format, fifoBytes, FrameCount);
        NATIVE_REQUIRE(fftPcm == reference,
                "PCM24 FFT conversion differed from the S32 swresample reference");
        NATIVE_REQUIRE(fftPcm.size() ==
                    static_cast<std::size_t>(FrameCount * 2 * sizeof(std::int16_t)),
                "PCM24 FFT conversion returned the wrong frame byte count");

        std::array<std::int16_t, FrameCount * 2> fftSamples{};
        std::memcpy(fftSamples.data(), fftPcm.data(), fftPcm.size());
        NATIVE_REQUIRE(fftSamples[0] > 0 && fftSamples[1] < 0 &&
                    fftSamples[2] > 0 && fftSamples[3] < 0,
                "PCM24 FFT conversion did not preserve stereo sample framing");
    }

    void TestFftSurroundResampleMatchesSwresample()
    {
        const auto verifyFormat = [](MusicPlayerLibrary::AudioChannelMode channelMode,
                                     std::span<const float> input)
        {
            const auto format = MakeResolvedOutputFormat(
                channelMode, MusicPlayerLibrary::AudioBitDepth::Bit32);
            NATIVE_REQUIRE(input.size() % format.channel_count == 0,
                    "surround FFT test input is not frame aligned");
            const int frameCount = static_cast<int>(input.size() / format.channel_count);
            MusicPlayerLibrary::FFTExecuter executor(format);
            const auto* inputBytes = reinterpret_cast<const std::uint8_t*>(input.data());
            const auto actual = executor.ResampleToFftFormat(inputBytes, frameCount);
            const auto expected = ReferenceSwrFftConversion(format, inputBytes, frameCount);
            NATIVE_REQUIRE(actual == expected,
                    "surround FFT conversion differed from S16 stereo libswresample reference");
        };

        const std::array<float, 12> fiveOne{
            0.8f, 0.4f, 0.6f, 1.0f, 0.2f, 0.3f,
            -0.3f, 0.7f, 0.1f, 0.5f, -0.6f, 0.9f};
        verifyFormat(MusicPlayerLibrary::AudioChannelMode::Surround51, fiveOne);

        const std::array<float, 16> sevenOne{
            0.8f, 0.4f, 0.6f, 1.0f, 0.2f, 0.3f, 0.5f, 0.7f,
            -0.3f, 0.7f, 0.1f, 0.5f, -0.6f, 0.9f, -0.2f, 0.4f};
        verifyFormat(MusicPlayerLibrary::AudioChannelMode::Surround71, sevenOne);
    }

    void TestFftLegacyCoreBehavior()
    {
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48000);
        FFTExecuterProbe executor(format);

        NATIVE_REQUIRE(executor.FrameCount() == 2048,
                "legacy FFT frame size is no longer fixed at 2048");
        NATIVE_REQUIRE(executor.SampleRate() == 48000,
                "legacy FFT sample rate is no longer fixed at 48 kHz");

        constexpr std::array<std::size_t, 33> ExpectedBoundaries{
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
            11, 12, 14, 17, 21, 26, 33, 41, 51, 63,
            79, 98, 122, 151, 188, 233, 289, 359, 446,
            554, 687, 853
        };
        const auto boundaries = executor.Boundaries();
        NATIVE_REQUIRE(boundaries.size() == ExpectedBoundaries.size() &&
                    std::equal(boundaries.begin(), boundaries.end(),
                               ExpectedBoundaries.begin()),
                "legacy logarithmic FFT band boundaries changed");

        std::vector<std::int16_t> fullScalePcm(executor.FrameCount() * 2, 32767);
        std::vector<std::uint8_t> fullScaleBytes(executor.FrameByteCount());
        std::memcpy(
            fullScaleBytes.data(), fullScalePcm.data(), fullScaleBytes.size());
        const auto windowed = executor.Window(fullScaleBytes);
        NATIVE_REQUIRE(windowed.size() == executor.FrameCount(),
                "legacy FFT window returned the wrong frame count");
        RequireClose(0.0f, static_cast<float>(windowed.front()), 1.0e-7f,
                     "legacy FFT window no longer begins at zero");
        RequireClose(1.07668f,
                     static_cast<float>(windowed[executor.FrameCount() / 2]),
                     2.0e-4f,
                     "legacy FFT window coefficient changed");
        RequireClose(0.0f, static_cast<float>(windowed.back()), 1.0e-7f,
                     "legacy FFT window no longer ends at zero");

        const auto analyzeBins = [&executor](std::span<const int> bins,
                                              float amplitude)
        {
            std::vector<std::int16_t> pcm(executor.FrameCount() * 2);
            for (std::size_t frame = 0; frame < executor.FrameCount(); ++frame)
            {
                double sample = 0.0;
                for (const int bin : bins)
                {
                    sample += amplitude * std::sin(
                        2.0 * std::numbers::pi * static_cast<double>(bin) *
                        static_cast<double>(frame) /
                        static_cast<double>(executor.FrameCount()));
                }
                const auto quantized = static_cast<std::int16_t>(
                    std::lround(std::clamp(sample, -1.0, 1.0) * 32767.0));
                pcm[frame * 2] = quantized;
                pcm[frame * 2 + 1] = quantized;
            }
            std::vector<std::uint8_t> bytes(executor.FrameByteCount());
            std::memcpy(bytes.data(), pcm.data(), bytes.size());
            const auto segments = executor.Analyze(bytes);
            return *std::max_element(segments.begin(), segments.end());
        };

        constexpr std::array<int, 1> MidTone{96};
        constexpr std::array<int, 1> HighTone{300};
        constexpr std::array<int, 2> TwoHighTones{300, 340};
        const float ordinaryPeak = analyzeBins(MidTone, 0.1f);
        const float loudPeak = analyzeBins(MidTone, 0.5f);
        const float highPeak = analyzeBins(HighTone, 0.1f);
        const float twoHighPeak = analyzeBins(TwoHighTones, 0.1f);
        NATIVE_REQUIRE(ordinaryPeak > 0.68f && ordinaryPeak < 0.74f,
                "legacy raw-magnitude dB scaling changed");
        NATIVE_REQUIRE(loudPeak > 0.99f,
                "legacy FFT no longer clamps a loud mid-frequency tone");
        NATIVE_REQUIRE(highPeak < ordinaryPeak * 0.9f,
                "legacy high-frequency attenuation changed");
        NATIVE_REQUIRE(twoHighPeak < highPeak + 0.03f,
                "legacy FFT bands no longer use peak-bin aggregation");
    }

	void TestFftTimelineFollowsPresentationCursor()
	{
		constexpr int SampleRate = 48000;
		constexpr int TotalFrames = 8192;
		constexpr int ToneChangeFrame = 4096;
		constexpr int LowBin = 20;
		constexpr int HighBin = 300;
		constexpr int FftFrames = 2048;
		constexpr int HopFrames = 768;
		constexpr int FirstAllHighWindowEnd = FftFrames + 6 * HopFrames;
		const auto format = MakeResolvedOutputFormat(
			MusicPlayerLibrary::AudioChannelMode::Stereo,
			MusicPlayerLibrary::AudioBitDepth::Bit16,
			SampleRate);

		std::vector<std::int16_t> pcm(static_cast<std::size_t>(TotalFrames) * 2);
		for (int frame = 0; frame < TotalFrames; ++frame)
		{
			const int bin = frame < ToneChangeFrame ? LowBin : HighBin;
			const double sample = 0.25 * std::sin(
				2.0 * std::numbers::pi * static_cast<double>(bin) *
				static_cast<double>(frame) / FftFrames);
			const auto quantized = static_cast<std::int16_t>(
				std::lround(sample * 32767.0));
			pcm[static_cast<std::size_t>(frame) * 2] = quantized;
			pcm[static_cast<std::size_t>(frame) * 2 + 1] = quantized;
		}

		FFTExecuterProbe burst(format);
		burst.AddTimelineSamples(
			reinterpret_cast<const std::uint8_t*>(pcm.data()),
			TotalFrames,
			0.0);

		FFTExecuterProbe chunked(format);
		constexpr int ChunkFrames = 256;
		for (int offset = 0; offset < TotalFrames; offset += ChunkFrames)
		{
			const int frameCount = (std::min)(ChunkFrames, TotalFrames - offset);
			chunked.AddTimelineSamples(
				reinterpret_cast<const std::uint8_t*>(
					pcm.data() + static_cast<std::size_t>(offset) * 2),
				frameCount,
				static_cast<double>(offset) / SampleRate);
		}

		auto copyAt = [](FFTExecuterProbe& executor, const int frame)
		{
			std::vector<float> result(32, -1.0f);
			const int copied = executor.CopyAudioFFTDataAt(
				result.data(), static_cast<int>(result.size()),
				static_cast<double>(frame) / SampleRate);
			result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
			return result;
		};

		NATIVE_REQUIRE(copyAt(burst, FftFrames - 1).empty(),
			"FFT exposed a future window before the presentation cursor reached it");
		const auto burstLow = copyAt(burst, FftFrames);
		const auto chunkedLow = copyAt(chunked, FftFrames);
		const auto burstBetweenWindows = copyAt(
			burst, FftFrames + HopFrames - 1);
		const auto burstHigh = copyAt(burst, FirstAllHighWindowEnd);
		const auto chunkedHigh = copyAt(chunked, FirstAllHighWindowEnd);

		NATIVE_REQUIRE(burstLow.size() == 32 && chunkedLow.size() == 32 &&
			burstBetweenWindows.size() == 32 &&
			burstHigh.size() == 32 && chunkedHigh.size() == 32,
			"timestamped FFT timeline did not return complete spectra");
		for (std::size_t band = 0; band < burstLow.size(); ++band)
		{
			RequireClose(burstLow[band], chunkedLow[band], 1.0e-6f,
				"prefetch burst changed the low-frequency spectrum at a fixed cursor");
			RequireClose(burstLow[band], burstBetweenWindows[band], 1.0e-6f,
				"cursor between FFT timestamps selected a future spectrum");
			RequireClose(burstHigh[band], chunkedHigh[band], 1.0e-6f,
				"PCM chunking changed the high-frequency spectrum at a fixed cursor");
		}
		const auto lowPeak = static_cast<std::size_t>(std::distance(
			burstLow.begin(), std::max_element(burstLow.begin(), burstLow.end())));
		const auto highPeak = static_cast<std::size_t>(std::distance(
			burstHigh.begin(), std::max_element(burstHigh.begin(), burstHigh.end())));
		NATIVE_REQUIRE(lowPeak != highPeak,
			"presentation cursor did not advance from low to high-frequency PCM");

		burst.ResetBuffers();
		NATIVE_REQUIRE(copyAt(burst, TotalFrames).empty(),
			"FFT reset retained a spectrum from the previous generation");
	}

	void TestFftObserverPublishesGenerationAfterReset()
	{
		constexpr std::uint32_t FrameCount = 4096;
		const auto format = MakeResolvedOutputFormat(
			MusicPlayerLibrary::AudioChannelMode::Stereo,
			MusicPlayerLibrary::AudioBitDepth::Bit16,
			48000);
		FFTAudioObserve observer(format);
		observer.OnFormat(format, 1);

		std::vector<std::int16_t> pcm(static_cast<std::size_t>(FrameCount) * 2);
		for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
		{
			const auto sample = static_cast<std::int16_t>(std::lround(
				0.25 * std::sin(2.0 * std::numbers::pi * 40.0 * frame / 2048.0) *
				32767.0));
			pcm[static_cast<std::size_t>(frame) * 2] = sample;
			pcm[static_cast<std::size_t>(frame) * 2 + 1] = sample;
		}
		const NormalizedPcmBlock block{
			.bytes = std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(pcm.data()),
				pcm.size() * sizeof(std::int16_t)),
			.frame_count = FrameCount,
			.stream_frame_offset = 0,
			.generation = 1
		};
		observer.OnPcm(block);

		std::array<float, 32> spectrum{};
		int copied = 0;
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(1);
		while (copied == 0 && std::chrono::steady_clock::now() < deadline)
		{
			copied = observer.CopySpectrum(
				spectrum.data(), static_cast<int>(spectrum.size()), 1, FrameCount);
			if (copied == 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		NATIVE_REQUIRE(copied == static_cast<int>(spectrum.size()),
			"FFT observer did not publish the initial generation spectrum");

		// OnFormat and OnReset are separate interface callbacks. Publishing the
		// new generation must itself clear the old timeline so a concurrent UI
		// read in the interval between those callbacks cannot see stale data.
		observer.OnFormat(format, 2);
		NATIVE_REQUIRE(observer.CopySpectrum(
				spectrum.data(), static_cast<int>(spectrum.size()), 2, FrameCount) == 0,
			"FFT observer exposed the previous timeline under a new generation");
	}

    void TestAudioPipelineRoutesNormalizedPcmBeforeSinkEffects()
    {
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48000);
        auto device = std::make_shared<FakeOutputDevice>();
        device->identity = 41;
        auto sink = std::make_shared<FakeAudioSink>(format, device);
        auto observer = std::make_shared<RecordingAudioObserve>();
        ContractAudioSource source(format);
        source.Connect(sink);
        source.Subscribe(observer);
        source.Subscribe(observer); // Subscriptions are identity-idempotent.

        const auto generation = source.BeginNormalizedStream();
        NATIVE_REQUIRE(generation == 1 && source.Generation() == generation,
                "source did not adopt the sink's first stream generation");
        NATIVE_REQUIRE(observer->format_generations ==
                    std::vector<AudioStreamGeneration>{generation} &&
                    observer->reset_generations ==
                    std::vector<AudioStreamGeneration>{generation},
                "observer did not receive exactly one format/reset for the generation");

        const std::array<std::uint8_t, 8> firstPcm{
            0x10, 0x11, 0x20, 0x21, 0x30, 0x31, 0x40, 0x41};
        const std::array<std::uint8_t, 4> finalPcm{0x50, 0x51, 0x60, 0x61};
        NATIVE_REQUIRE(source.EmitNormalized(firstPcm, 2, 0.0, false),
                "source rejected the first normalized PCM block");
        NATIVE_REQUIRE(source.EmitNormalized(finalPcm, 1, 2.0 / 48000.0, true),
                "source rejected the final normalized PCM block");

        const auto& submitted = sink->Submitted();
        NATIVE_REQUIRE(submitted.size() == 3,
                "sink did not receive normalized PCM plus an explicit EOS marker");
        NATIVE_REQUIRE(!submitted[0].end_of_stream &&
                    !submitted[1].end_of_stream && submitted[2].end_of_stream &&
                    std::count_if(
                        submitted.begin(), submitted.end(),
                        [](const CapturedPcmBlock& block)
                        {
                            return block.end_of_stream;
                        }) == 1,
                "only the explicit sink marker must carry end-of-stream");
        NATIVE_REQUIRE(submitted.front().generation == generation &&
                    submitted.back().generation == generation,
                "sink blocks did not retain their source generation");

        NATIVE_REQUIRE(observer->pcm_blocks.size() == 2 &&
                    observer->pcm_blocks[0].bytes ==
                        std::vector<std::uint8_t>(firstPcm.begin(), firstPcm.end()) &&
                    observer->pcm_blocks[1].bytes ==
                        std::vector<std::uint8_t>(finalPcm.begin(), finalPcm.end()),
                "observer did not receive the normalized source PCM byte-for-byte");
		NATIVE_REQUIRE(observer->pcm_blocks[0].stream_frame_offset == 0 &&
			observer->pcm_blocks[1].stream_frame_offset == 2 &&
			submitted[0].stream_frame_offset == 0 &&
			submitted[1].stream_frame_offset == 2,
			"normalized PCM did not retain a continuous generation-relative timeline");
        NATIVE_REQUIRE(!observer->pcm_blocks[0].end_of_stream &&
                    !observer->pcm_blocks[1].end_of_stream &&
                    observer->eos_generations ==
                        std::vector<AudioStreamGeneration>{generation},
                "observer EOS must be a generation-scoped event, separate from PCM");
        NATIVE_REQUIRE(sink->PostProcessed().size() == 3 &&
                    sink->PostProcessed()[0].bytes != observer->pcm_blocks[0].bytes &&
                    sink->PostProcessed()[1].bytes != observer->pcm_blocks[1].bytes,
                "fake sink did not model post-processing after observation");
        NATIVE_REQUIRE(!sink->GetState().stream_ended &&
                    !sink->WaitForStreamEnd(std::chrono::milliseconds(0)),
                "submitting the EOS block must not impersonate the output callback");
        NATIVE_REQUIRE(sink->SignalStreamEnd(generation) &&
                    sink->WaitForStreamEnd(std::chrono::milliseconds(0)),
                "matching EOS callback did not complete the stream");
        const auto endedState = sink->GetState();
        NATIVE_REQUIRE(endedState.stream_ended &&
                    endedState.generation == generation &&
                    endedState.buffers_queued == 0 &&
                    endedState.queued_frames == 0 &&
					endedState.samples_played == 0 &&
					endedState.media_frames_presented == 0 &&
					endedState.presentation_latency_frames == 0,
                "completed output stream did not reset its session state");
    }

    void TestAudioPipelineRejectsStaleGenerations()
    {
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48000);
        auto sink = std::make_shared<FakeAudioSink>(
            format, std::make_shared<FakeOutputDevice>());
        auto observer = std::make_shared<RecordingAudioObserve>();
        ContractAudioSource source(format);
        source.Connect(sink);
        source.Subscribe(observer);

        const auto firstGeneration = source.BeginNormalizedStream();
        const std::array<std::uint8_t, 4> firstPcm{1, 2, 3, 4};
        NATIVE_REQUIRE(source.EmitNormalized(firstPcm, 1, 0.0, false),
                "first generation rejected valid normalized PCM");

        const auto secondGeneration = source.BeginNormalizedStream();
        NATIVE_REQUIRE(secondGeneration == firstGeneration + 1,
                "BeginStream did not advance the stream generation");
        const std::array<std::uint8_t, 4> stalePcm{0xE1, 0xE2, 0xE3, 0xE4};
        const NormalizedPcmBlock staleBlock{
            .bytes = stalePcm,
            .frame_count = 1,
            .generation = firstGeneration,
            .end_of_stream = true
        };
        NATIVE_REQUIRE(!sink->Submit(staleBlock),
                "sink accepted PCM from a replaced stream generation");
        NATIVE_REQUIRE(!sink->SignalStreamEnd(firstGeneration) &&
                    !sink->GetState().stream_ended,
                "late EOS from a replaced generation completed the active stream");

        const std::array<std::uint8_t, 4> currentPcm{5, 6, 7, 8};
        NATIVE_REQUIRE(source.EmitNormalized(currentPcm, 1, 1.0, true),
                "active generation rejected its final normalized PCM");
        NATIVE_REQUIRE(!source.EmitNormalized(currentPcm, 1, 1.1, false),
                "source accepted PCM after publishing its generation EOS");
        NATIVE_REQUIRE(sink->SignalStreamEnd(secondGeneration) &&
                    sink->GetState().stream_ended,
                "active generation EOS did not complete its stream");

        const std::vector<AudioStreamGeneration> expectedGenerations{
            firstGeneration, secondGeneration};
        NATIVE_REQUIRE(observer->format_generations == expectedGenerations &&
                    observer->reset_generations == expectedGenerations &&
                    observer->eos_generations ==
                    std::vector<AudioStreamGeneration>{secondGeneration},
                "observer lifecycle events were not isolated by generation");
        NATIVE_REQUIRE(std::none_of(
                    sink->Submitted().begin(), sink->Submitted().end(),
                    [&stalePcm](const CapturedPcmBlock& block)
                    {
                        return block.bytes == std::vector<std::uint8_t>(
                            stalePcm.begin(), stalePcm.end());
                    }),
                "stale-generation PCM reached the sink queue");
    }

    void TestAudioSinkCreatesExplicitEosForEmptyStream()
    {
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48000);
        FakeAudioSink sink(format, std::make_shared<FakeOutputDevice>());
        const auto generation = sink.BeginStream();

        NATIVE_REQUIRE(sink.EndStream(),
                "empty stream did not synthesize an explicit EOS marker");
        NATIVE_REQUIRE(!sink.EndStream(),
                "sink accepted a duplicate EOS marker for the same generation");
        NATIVE_REQUIRE(sink.Submitted().size() == 1,
                "empty stream submitted more than one EOS marker");
        const auto& marker = sink.Submitted().front();
        NATIVE_REQUIRE(marker.end_of_stream &&
                    marker.generation == generation &&
                    marker.frame_count == 1 &&
                    marker.bytes.size() ==
                        format.wave_format.Format.nBlockAlign &&
                    std::all_of(
                        marker.bytes.begin(), marker.bytes.end(),
                        [](const std::uint8_t value) { return value == 0; }),
                "empty-stream EOS marker was not one silent normalized frame");
        NATIVE_REQUIRE(!sink.GetState().stream_ended,
                "EOS submission completed an empty stream before its callback");
        NATIVE_REQUIRE(sink.SignalStreamEnd(generation) &&
                    sink.GetState().stream_ended,
                "empty-stream EOS callback did not complete the generation");
    }

    void TestAudioSinksShareDeviceButKeepEosStateIndependent()
    {
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48000);
        auto device = std::make_shared<FakeOutputDevice>();
        device->identity = 73;
        auto firstSink = std::make_shared<FakeAudioSink>(format, device);
        auto secondSink = std::make_shared<FakeAudioSink>(format, device);
        NATIVE_REQUIRE(firstSink->Device() == secondSink->Device() &&
                    firstSink->Device()->identity == 73,
                "sinks did not retain the same application-scoped output device");

        ContractAudioSource firstSource(format);
        ContractAudioSource secondSource(format);
        firstSource.Connect(firstSink);
        secondSource.Connect(secondSink);
        const auto firstGeneration = firstSource.BeginNormalizedStream();
        const auto secondGeneration = secondSource.BeginNormalizedStream();
        const std::array<std::uint8_t, 4> firstPcm{10, 11, 12, 13};
        const std::array<std::uint8_t, 4> secondPcm{20, 21, 22, 23};
        NATIVE_REQUIRE(firstSource.EmitNormalized(firstPcm, 1, 0.0, true) &&
                    secondSource.EmitNormalized(secondPcm, 1, 0.0, true),
                "shared-device sinks did not accept independent source streams");

        NATIVE_REQUIRE(firstSink->SignalStreamEnd(firstGeneration),
                "first sink did not accept its EOS callback");
        NATIVE_REQUIRE(firstSink->GetState().stream_ended &&
                    !secondSink->GetState().stream_ended &&
                    secondSink->GetState().buffers_queued == 2,
                "one sink's EOS reset another sink sharing the device");
        firstSink->AbortStream();
        NATIVE_REQUIRE(!secondSink->GetState().stream_ended &&
                    secondSink->GetState().buffers_queued == 2,
                "aborting one sink disturbed another sink sharing the device");
        NATIVE_REQUIRE(secondSink->SignalStreamEnd(secondGeneration) &&
                    secondSink->GetState().stream_ended,
                "second shared-device sink could not complete independently");
    }

    void TestFAudioSinkSkipsFlatEqualizerAndCompletesEos()
    {
        AudioOutputFormat request{};
        request.requested_sample_rate = 48000;
        request.requested_channel_mode =
            MusicPlayerLibrary::AudioChannelMode::Stereo;
        request.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit32;
        auto sink = std::make_shared<FAudioSink>(request);
        sink->SetMasterVolume(0.0f);

        NATIVE_REQUIRE(!sink->IsLimiterEnabled(),
                "a flat equalizer unexpectedly enabled FAPO limiting");
        sink->SetEqualizerBand(5, 6);
        NATIVE_REQUIRE(sink->IsLimiterEnabled(),
                "an effective equalizer band did not enable the limiter");
        sink->SetEqualizerBand(5, 0);
        NATIVE_REQUIRE(!sink->IsLimiterEnabled(),
                "clearing every equalizer band did not disable the limiter");

        const auto generation = sink->BeginStream();
		constexpr std::uint32_t FrameCount = 256;
		const auto pcm = StereoSine(48000, 440.0f, 0.1f, FrameCount);
		const auto* pcmBytes = reinterpret_cast<const std::uint8_t*>(pcm.data());
		const NormalizedPcmBlock block{
			.bytes = std::span<const std::uint8_t>(
				pcmBytes, pcm.size() * sizeof(float)),
			.frame_count = FrameCount,
			.generation = generation,
			.end_of_stream = true
		};
		const bool submitted = sink->Submit(block);
		const auto queuedState = sink->GetState();
		const bool eosQueued = submitted;
        const bool started = eosQueued && sink->Start();
        const bool ended = started &&
            sink->WaitForStreamEnd(std::chrono::seconds(10));
        const auto state = sink->GetState();
        sink->SetMasterVolume(1.0f);

		NATIVE_REQUIRE(generation != 0 && submitted && eosQueued && started && ended,
				"disabled FAPO did not pass PCM through the direct EOS path");
		NATIVE_REQUIRE(queuedState.buffers_queued == 1 &&
				queuedState.queued_frames == FrameCount,
			"flat-EQ EOS appended audio frames beyond the final source PCM block");
        NATIVE_REQUIRE(state.stream_ended && state.generation == generation &&
                    state.error_code == 0,
                "flat-EQ stream did not complete its FAudio EOS callback");
    }

    void TestFAudioSinkDrainsTailThenCompletesActualEos()
    {
        AudioOutputFormat floatRequest{};
        floatRequest.requested_sample_rate = 48000;
        floatRequest.requested_channel_mode =
            MusicPlayerLibrary::AudioChannelMode::Stereo;
        floatRequest.requested_bit_depth =
            MusicPlayerLibrary::AudioBitDepth::Bit32;
        auto sink = std::make_shared<FAudioSink>(floatRequest);

        // A differently normalized sink must retain the same application-wide
        // engine/mastering device. Constructing it also covers FAudio's
        // PCM24-source -> internal-float FAPO lock path.
        AudioOutputFormat pcm24Request{};
        pcm24Request.requested_sample_rate = 192000;
        pcm24Request.requested_channel_mode =
            MusicPlayerLibrary::AudioChannelMode::Surround51;
        pcm24Request.requested_bit_depth =
            MusicPlayerLibrary::AudioBitDepth::Bit24;
        auto pcm24Sink = std::make_shared<FAudioSink>(
            pcm24Request, sink->GetDevice());
        NATIVE_REQUIRE(pcm24Sink->GetDevice() == sink->GetDevice(),
            "real FAudio sinks did not share one output device");
        NATIVE_REQUIRE(
            pcm24Sink->GetOutputFormat().sample_rate == 192000 &&
            pcm24Sink->GetOutputFormat().channel_count == 6 &&
            pcm24Sink->GetOutputFormat().bit_depth ==
                MusicPlayerLibrary::AudioBitDepth::Bit24,
            "PCM24 sink lost its independent normalized source-voice format");
        pcm24Sink.reset();

        sink->SetMasterVolume(0.0f);
        sink->SetEqualizerBand(0, 24);
        const auto generation = sink->BeginStream();
        constexpr std::uint32_t FrameCount = 256;
        std::vector<float> impulse(static_cast<std::size_t>(FrameCount) * 2, 0.0f);
        impulse[0] = 0.25f;
        impulse[1] = 0.25f;
        const auto* impulseBytes = reinterpret_cast<const std::uint8_t*>(
            impulse.data());
        const NormalizedPcmBlock block{
            .bytes = std::span<const std::uint8_t>(
                impulseBytes, impulse.size() * sizeof(float)),
            .frame_count = FrameCount,
            .generation = generation,
			.end_of_stream = true
		};
		const bool submitted = sink->Submit(block);
		const bool eosQueued = submitted;
		// Clearing the final band during terminal drain must not disable the
		// FAPO before its process-sequence barrier completes.
		sink->SetEqualizerBand(0, 0);
		const bool limiterHeldThroughDrain = sink->IsLimiterEnabled();
		const bool started = eosQueued && sink->Start();
		bool presentationClockAdvanced = false;
		bool presentationClockMonotonic = true;
		std::uint64_t previousPresentedFrames = 0;
		std::uint32_t observedPresentationLatency = 0;
		const auto clockDeadline = std::chrono::steady_clock::now() +
			std::chrono::seconds(2);
		while (started && std::chrono::steady_clock::now() < clockDeadline)
		{
			const auto activeState = sink->GetState();
			if (activeState.stream_ended)
				break;
			presentationClockMonotonic = presentationClockMonotonic &&
				activeState.media_frames_presented >= previousPresentedFrames;
			previousPresentedFrames = activeState.media_frames_presented;
			observedPresentationLatency = (std::max)(
				observedPresentationLatency,
				activeState.presentation_latency_frames);
			presentationClockAdvanced = presentationClockAdvanced ||
				activeState.media_frames_presented > 0;
			if (presentationClockAdvanced &&
				activeState.samples_played >= FrameCount)
			{
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const bool ended = started &&
			sink->WaitForStreamEnd(std::chrono::seconds(10));
        const auto state = sink->GetState();
        sink->SetMasterVolume(1.0f);

		NATIVE_REQUIRE(submitted && eosQueued && started,
			"real FAudio sink did not accept PCM, drain request, and Start");
		NATIVE_REQUIRE(limiterHeldThroughDrain,
			"terminal drain disabled its FAPO before the tail barrier completed");
		NATIVE_REQUIRE(presentationClockAdvanced && presentationClockMonotonic &&
			observedPresentationLatency > 0,
			"real FAudio sink did not expose a monotonic device-latency-adjusted clock");
        NATIVE_REQUIRE(ended && state.stream_ended &&
                    state.generation == generation && state.error_code == 0,
            "real FAudio callbacks did not complete the drained EOS generation");
		NATIVE_REQUIRE(state.buffers_queued == 0 &&
					state.queued_frames == 0 && state.samples_played == 0 &&
					state.media_frames_presented == 0 &&
					state.presentation_latency_frames == 0,
			"real EOS callback did not reset completed sink state");
		const auto bypassGeneration = sink->BeginStream();
		NATIVE_REQUIRE(bypassGeneration > generation && !sink->IsLimiterEnabled(),
			"deferred flat EQ controls were not published at the next stream boundary");
		sink->AbortStream();
    }

	void TestWasapiClockHealthAcceptsNormalRate()
	{
		MusicPlayerLibrary::WasapiClockHealthMonitor monitor;
		constexpr std::uint64_t Frequency = 48'000;
		constexpr std::uint64_t QpcStart = 50'000'000;

		NATIVE_REQUIRE(!monitor.Observe(0, Frequency, QpcStart),
			"the first WASAPI clock sample requested recovery");
		NATIVE_REQUIRE(!monitor.Observe(12'000, Frequency, QpcStart + 2'500'000) &&
			!monitor.Observe(24'000, Frequency, QpcStart + 5'000'000),
			"a normally advancing 48 kHz WASAPI clock requested recovery");
	}

	void TestWasapiClockHealthIgnoresShortJitter()
	{
		MusicPlayerLibrary::WasapiClockHealthMonitor monitor;
		constexpr std::uint64_t Frequency = 48'000;
		constexpr std::uint64_t PositionStart = 1'000'000;
		constexpr std::uint64_t QpcStart = 70'000'000;

		NATIVE_REQUIRE(!monitor.Observe(PositionStart, Frequency, QpcStart) &&
			!monitor.Observe(
				PositionStart + 960, Frequency, QpcStart + 1'000'000),
			"a sub-window WASAPI scheduling jitter requested recovery");
		NATIVE_REQUIRE(!monitor.Observe(
				PositionStart + 12'000, Frequency, QpcStart + 2'500'000) &&
			!monitor.Observe(
				PositionStart + 24'000, Frequency, QpcStart + 5'000'000),
			"a short WASAPI clock jitter was not allowed to catch up");
	}

	void TestWasapiClockHealthSupports44100Hz()
	{
		MusicPlayerLibrary::WasapiClockHealthMonitor monitor;
		constexpr std::uint64_t Frequency = 44'100;
		constexpr std::uint64_t QpcStart = 90'000'000;

		NATIVE_REQUIRE(!monitor.Observe(0, Frequency, QpcStart) &&
			!monitor.Observe(11'025, Frequency, QpcStart + 2'500'000) &&
			!monitor.Observe(22'050, Frequency, QpcStart + 5'000'000),
			"a normally advancing 44.1 kHz WASAPI clock requested recovery");
	}

	void TestWasapiClockHealthRequiresTwoSlowWindows()
	{
		MusicPlayerLibrary::WasapiClockHealthMonitor monitor;
		constexpr std::uint64_t Frequency = 48'000;
		constexpr std::uint64_t QpcStart = 110'000'000;

		NATIVE_REQUIRE(!monitor.Observe(0, Frequency, QpcStart) &&
			!monitor.Observe(7'200, Frequency, QpcStart + 2'500'000),
			"one slow WASAPI clock window requested recovery");
		NATIVE_REQUIRE(!monitor.Observe(
				19'200, Frequency, QpcStart + 5'000'000) &&
			!monitor.Observe(
				26'400, Frequency, QpcStart + 7'500'000),
			"a healthy WASAPI clock window did not break the slow-window sequence");
		NATIVE_REQUIRE(monitor.Observe(
				33'600, Frequency, QpcStart + 10'000'000),
			"two consecutive 60 percent WASAPI clock windows did not request recovery");
	}

	void TestWasapiClockHealthResetClearsSlowHistory()
	{
		MusicPlayerLibrary::WasapiClockHealthMonitor monitor;
		constexpr std::uint64_t Frequency = 48'000;
		constexpr std::uint64_t QpcStart = 130'000'000;

		NATIVE_REQUIRE(!monitor.Observe(0, Frequency, QpcStart) &&
			!monitor.Observe(7'200, Frequency, QpcStart + 2'500'000) &&
			monitor.Observe(14'400, Frequency, QpcStart + 5'000'000),
			"the reset test did not first reach the recovery threshold");
		monitor.Reset();
		NATIVE_REQUIRE(!monitor.Observe(0, Frequency, QpcStart + 5'100'000) &&
			!monitor.Observe(12'000, Frequency, QpcStart + 7'600'000) &&
			!monitor.Observe(24'000, Frequency, QpcStart + 10'100'000),
			"a reset WASAPI clock monitor retained stale slow-window history");
	}

	void TestWasapiMixFormatConversion()
	{
		WAVEFORMATEX pcm16{};
		pcm16.wFormatTag = WAVE_FORMAT_PCM;
		pcm16.nChannels = 2;
		pcm16.nSamplesPerSec = 44'100;
		pcm16.wBitsPerSample = 16;
		pcm16.nBlockAlign = 4;
		pcm16.nAvgBytesPerSec = pcm16.nSamplesPerSec * pcm16.nBlockAlign;
		FAudioWaveFormatExtensible converted{};
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
				&pcm16, converted),
			"legacy PCM16 endpoint mix format was rejected");
		const auto pcm16Resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
			AudioOutputFormat{}, converted);
		NATIVE_REQUIRE(
			pcm16Resolved.sample_rate == 44'100 &&
			pcm16Resolved.channel_count == 2 &&
			pcm16Resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit16,
			"legacy PCM16 endpoint mix format was not preserved");

		WAVEFORMATEX float32 = pcm16;
		float32.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		float32.nSamplesPerSec = 48'000;
		float32.wBitsPerSample = 32;
		float32.nBlockAlign = 8;
		float32.nAvgBytesPerSec =
			float32.nSamplesPerSec * float32.nBlockAlign;
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
				&float32, converted) &&
			MusicPlayerLibrary::GuidEquals(
				converted.SubFormat,
				MusicPlayerLibrary::IeeeFloatAudioSubFormat),
			"legacy float32 endpoint mix format was rejected");

		AudioOutputFormat pcm24Request{};
		pcm24Request.requested_sample_rate = 96'000;
		pcm24Request.requested_channel_mode =
			MusicPlayerLibrary::AudioChannelMode::Surround51;
		pcm24Request.requested_bit_depth =
			MusicPlayerLibrary::AudioBitDepth::Bit24;
		const auto pcm24Expected = MusicPlayerLibrary::ResolveAudioOutputFormat(
			pcm24Request, MusicPlayerLibrary::MakeFallbackAudioWaveFormat());
		auto pcm24Windows =
			MusicPlayerLibrary::ToWindowsWaveFormatExtensible(pcm24Expected);
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
				&pcm24Windows.Format, converted) &&
			converted.Format.cbSize ==
				MusicPlayerLibrary::FAudioExtensibleFormatExtraSize &&
			converted.Samples.wValidBitsPerSample == 24 &&
			converted.dwChannelMask == pcm24Expected.channel_mask,
			"extensible PCM24-in-32 endpoint mix format was not preserved");
		const auto pcm24Resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
			AudioOutputFormat{}, converted);
		NATIVE_REQUIRE(
			MusicPlayerLibrary::AreResolvedPcmFormatsEqual(
				pcm24Expected, pcm24Resolved),
			"converted PCM24-in-32 endpoint mix format changed PCM geometry");

		auto malformed = pcm24Windows;
		malformed.Format.cbSize = static_cast<WORD>(
			sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) - 1);
		NATIVE_REQUIRE(
			!MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
				&malformed.Format, converted),
			"undersized extensible endpoint mix format was accepted");
		malformed = pcm24Windows;
		malformed.SubFormat.Data1 ^= 1u;
		NATIVE_REQUIRE(
			!MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
				&malformed.Format, converted),
			"unknown extensible endpoint subformat was accepted");
		malformed = pcm24Windows;
		malformed.dwChannelMask = SPEAKER_FRONT_LEFT;
		NATIVE_REQUIRE(
			!MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
				&malformed.Format, converted),
			"endpoint mix format with inconsistent channel mask was accepted");
	}

	void TestWasapiExclusiveProbeFallbacksAndWireVariants()
	{
		AudioOutputFormat requested{};
		requested.requested_sample_rate = 96'000;
		requested.requested_channel_mode =
			MusicPlayerLibrary::AudioChannelMode::Surround51;
		requested.requested_bit_depth =
			MusicPlayerLibrary::AudioBitDepth::Bit32;

		AudioOutputFormat preferred_request{};
		preferred_request.requested_sample_rate = 48'000;
		preferred_request.requested_channel_mode =
			MusicPlayerLibrary::AudioChannelMode::Stereo;
		preferred_request.requested_bit_depth =
			MusicPlayerLibrary::AudioBitDepth::Bit32;
		const auto preferred = MusicPlayerLibrary::ResolveAudioOutputFormat(
			preferred_request, MusicPlayerLibrary::MakeFallbackAudioWaveFormat());
		const auto axes = MusicPlayerLibrary::BuildWasapiExclusiveProbeAxes(
			requested, preferred);

		NATIVE_REQUIRE(
			!axes.sample_rates.empty() && axes.sample_rates.front() == 96'000 &&
			std::find(axes.sample_rates.begin(), axes.sample_rates.end(), 48'000) !=
				axes.sample_rates.end() &&
			std::find(axes.sample_rates.begin(), axes.sample_rates.end(), 44'100) !=
				axes.sample_rates.end(),
			"an explicit WASAPI sample rate suppressed safe fallback rates");
		NATIVE_REQUIRE(
			!axes.channel_modes.empty() && axes.channel_modes.front() ==
				MusicPlayerLibrary::AudioChannelMode::Surround51 &&
			std::find(
				axes.channel_modes.begin(), axes.channel_modes.end(),
				MusicPlayerLibrary::AudioChannelMode::Stereo) !=
				axes.channel_modes.end(),
			"an explicit WASAPI channel mode suppressed the stereo fallback");
		NATIVE_REQUIRE(
			!axes.bit_depths.empty() && axes.bit_depths.front() ==
				MusicPlayerLibrary::AudioBitDepth::Bit32 &&
			std::find(
				axes.bit_depths.begin(), axes.bit_depths.end(),
				MusicPlayerLibrary::AudioBitDepth::Bit24) !=
				axes.bit_depths.end() &&
			std::find(
				axes.bit_depths.begin(), axes.bit_depths.end(),
				MusicPlayerLibrary::AudioBitDepth::Bit16) !=
				axes.bit_depths.end(),
			"an explicit WASAPI bit depth suppressed integer PCM fallbacks");

		AudioOutputFormat pcm16_request = preferred_request;
		pcm16_request.requested_bit_depth =
			MusicPlayerLibrary::AudioBitDepth::Bit16;
		const auto pcm16 = MusicPlayerLibrary::ResolveAudioOutputFormat(
			pcm16_request, MusicPlayerLibrary::MakeFallbackAudioWaveFormat());
		AudioOutputFormat legacy_pcm16{};
		NATIVE_REQUIRE(
			MusicPlayerLibrary::TryMakeWasapiLegacyVariant(
				pcm16, legacy_pcm16) &&
			legacy_pcm16.wave_format.Format.wFormatTag == WAVE_FORMAT_PCM &&
			legacy_pcm16.wave_format.Format.cbSize == 0,
			"stereo PCM16 did not produce a legacy WASAPI descriptor");
		const auto extensible_pcm16_wave =
			MusicPlayerLibrary::ToWindowsWaveFormatExtensible(pcm16);
		const auto legacy_pcm16_wave =
			MusicPlayerLibrary::ToWindowsWaveFormatExtensible(legacy_pcm16);
		NATIVE_REQUIRE(
			!MusicPlayerLibrary::AreWasapiWireFormatsEqual(
				extensible_pcm16_wave, legacy_pcm16_wave),
			"legacy and extensible PCM16 descriptors were incorrectly deduplicated");

		AudioOutputFormat surround_request = pcm16_request;
		surround_request.requested_channel_mode =
			MusicPlayerLibrary::AudioChannelMode::Surround51;
		const auto surround = MusicPlayerLibrary::ResolveAudioOutputFormat(
			surround_request, MusicPlayerLibrary::MakeFallbackAudioWaveFormat());
		AudioOutputFormat invalid_legacy{};
		NATIVE_REQUIRE(
			!MusicPlayerLibrary::TryMakeWasapiLegacyVariant(
				surround, invalid_legacy),
			"a multichannel WASAPI descriptor was downgraded to legacy WAVEFORMATEX");

		const auto pcm32 = MusicPlayerLibrary::MakeWasapiPcm32Variant(preferred);
		NATIVE_REQUIRE(
			pcm32.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit32 &&
			pcm32.sample_format == AV_SAMPLE_FMT_S32 &&
			MusicPlayerLibrary::GuidEquals(
				pcm32.wave_format.SubFormat,
				MusicPlayerLibrary::PcmAudioSubFormat) &&
			!MusicPlayerLibrary::AreWasapiWireFormatsEqual(
				MusicPlayerLibrary::ToWindowsWaveFormatExtensible(preferred),
				MusicPlayerLibrary::ToWindowsWaveFormatExtensible(pcm32)),
			"integer PCM32 and float32 WASAPI descriptors collapsed together");
	}

	void TestPcm32IntegerConversion()
	{
		constexpr std::array<float, 5> Samples{
			-1.0f, -0.5f, 0.0f, 0.5f, 1.0f
		};
		std::array<std::uint8_t, Samples.size() * sizeof(std::int32_t)> bytes{};
		NATIVE_REQUIRE(
			EncodePcmSamples(
				Samples, bytes,
				MusicPlayerLibrary::AudioBitDepth::Bit32,
				AV_SAMPLE_FMT_S32),
			"float samples could not be encoded as signed PCM32");

		std::array<std::int32_t, Samples.size()> encoded{};
		std::memcpy(encoded.data(), bytes.data(), bytes.size());
		NATIVE_REQUIRE(
			encoded[0] == (std::numeric_limits<std::int32_t>::min)() &&
			encoded[1] == -1'073'741'824 && encoded[2] == 0 &&
			encoded[3] == 1'073'741'824 &&
			encoded[4] == (std::numeric_limits<std::int32_t>::max)(),
			"signed PCM32 encoding did not use the full integer range");

		std::array<float, Samples.size()> decoded{};
		NATIVE_REQUIRE(
			DecodePcmSamples(
				bytes, decoded,
				MusicPlayerLibrary::AudioBitDepth::Bit32,
				AV_SAMPLE_FMT_S32),
			"signed PCM32 samples could not be decoded");
		for (std::size_t index = 0; index < Samples.size(); ++index)
		{
			NATIVE_REQUIRE(
				std::abs(decoded[index] - Samples[index]) <= 1.0e-6f,
				"signed PCM32 conversion changed normalized sample amplitude");
		}

		std::array<std::uint8_t, Samples.size() * sizeof(float)> float_bytes{};
		NATIVE_REQUIRE(
			EncodePcmSamples(
				Samples, float_bytes,
				MusicPlayerLibrary::AudioBitDepth::Bit32,
				AV_SAMPLE_FMT_FLT) &&
			DecodePcmSamples(
				float_bytes, decoded,
				MusicPlayerLibrary::AudioBitDepth::Bit32,
				AV_SAMPLE_FMT_FLT),
			"the existing float32 conversion path regressed");
	}

    void TestLrcTimestampFractionNormalization()
    {
        using MusicPlayerLibrary::LrcTextParser::TryParseTimestamp;
        struct TimestampCase
        {
            std::string_view text;
            int expected_ms;
        };
        constexpr std::array Cases{
            TimestampCase{"01:02", 62'000},
            TimestampCase{"01:02.3", 62'300},
            TimestampCase{"01:02.34", 62'340},
            TimestampCase{"01:02.345", 62'345},
            TimestampCase{"01:02.3456", 62'345},
        };

        for (const auto& test_case : Cases)
        {
            const auto parsed = TryParseTimestamp(test_case.text);
            NATIVE_REQUIRE(parsed && *parsed == test_case.expected_ms,
                "LRC timestamp fraction was not normalized to milliseconds");
        }
        NATIVE_REQUIRE(!TryParseTimestamp("01:02.34567"),
            "LRC timestamp accepted more than four fractional digits");
        NATIVE_REQUIRE(!TryParseTimestamp("01:02.trailing"),
            "LRC timestamp accepted a non-numeric fractional component");
    }

    void TestLrcProgressDelimiterParsing()
    {
        using MusicPlayerLibrary::LrcTextParser::TryParseProgressLine;

        const auto angle = TryParseProgressLine(
            "A<00:00>B<00:00.1>C<00:00.25>D<00:00.375>E<00:00.5000>");
        NATIVE_REQUIRE(angle && angle->plain_text == "ABCDE" &&
                angle->segments.size() == 5 &&
                angle->segments[0].end_time_ms == 0 &&
                angle->segments[1].end_time_ms == 100 &&
                angle->segments[2].end_time_ms == 250 &&
                angle->segments[3].end_time_ms == 375 &&
                angle->segments[4].end_time_ms == 500,
            "angle-bracket progress nodes were parsed inconsistently");

        const auto square = TryParseProgressLine("你[00:00.250]好[00:00.500]");
        NATIVE_REQUIRE(square && square->plain_text == "你好" &&
                square->segments.size() == 2,
            "square-bracket progress nodes were not parsed");
        NATIVE_REQUIRE(!TryParseProgressLine("A<00:00.250]"),
            "angle progress node accepted a mismatched closing bracket");
        NATIVE_REQUIRE(!TryParseProgressLine("A[00:00.250>"),
            "square progress node accepted a mismatched closing bracket");
    }

    void TestLrcProgressMultiLinePlainTextAndControllerSelection()
    {
        const std::vector<std::string> lines{
            "plain translation",
            "A<00:00.100>B<00:00.200>",
            "X[00:00.300]",
        };
        const auto parsed = MusicPlayerLibrary::LrcTextParser::ParseProgressLines(lines);
        const std::vector<std::string> expected_plain_texts{
            "plain translation", "AB", "X"
        };
        NATIVE_REQUIRE(parsed.plain_texts == expected_plain_texts,
            "multi-line progress parsing did not retain all plain-text lines");
        NATIVE_REQUIRE(parsed.controller_line &&
                parsed.controller_line_index == 1 &&
                parsed.controller_line->segments.size() == 2,
            "multi-line progress parsing did not select the first richest controller line");
    }

#define NATIVE_TEST_CASE(suite, name, function) \
    TEST_CASE(name * doctest::test_suite(suite)) { function(); }

    NATIVE_TEST_CASE("eq", "zero dB identity", TestZeroDbIdentity)
    NATIVE_TEST_CASE("eq", "snapshot pre-gain is DSP-owned", TestSinkPreGainIsAppliedInsideDsp)
    NATIVE_TEST_CASE("eq", "1 kHz +6 dB", TestOneKilohertzPlusSixDb)
    NATIVE_TEST_CASE("eq", "Nyquist and configured rates", TestNyquistAndConfiguredRates)
    NATIVE_TEST_CASE("limiter", "adaptive input loudness matching", TestAdaptiveLimiterMatchesInputLoudness)
	NATIVE_TEST_CASE("limiter", "adaptive reset clears loudness history", TestAdaptiveLimiterResetClearsLoudnessHistory)
	NATIVE_TEST_CASE("limiter", "adaptive stacked-band loudness matching", TestAdaptiveLimiterMatchesStackedEqualizerBands)
    NATIVE_TEST_CASE("limiter", "configured look-ahead delay", TestLimiterDelayAtConfiguredRates)
    NATIVE_TEST_CASE("limiter", "linked stereo ceiling", TestLimiterLinksStereoAtCeiling)
    NATIVE_TEST_CASE("limiter", "no make-up gain", TestLimiterDoesNotApplyMakeupGain)
    NATIVE_TEST_CASE("limiter", "linear release", TestLimiterReleaseIsLinear)
    NATIVE_TEST_CASE("limiter", "silent tail drain", TestLimiterSilentInputDrainsTail)
    NATIVE_TEST_CASE("limiter", "reset generation drops tail", TestLimiterResetGenerationDropsTail)
    NATIVE_TEST_CASE("limiter", "chunk invariance", TestLimiterIsChunkInvariant)
    NATIVE_TEST_CASE("fapo", "factory and registration", TestFapoFactoryAndRegistration)
    NATIVE_TEST_CASE("fapo", "extensible formats advertised", TestFapoAdvertisesExtensibleFormats)
    NATIVE_TEST_CASE("fapo", "in-place matches out-of-place", TestFapoInPlaceMatchesOutOfPlace)
    NATIVE_TEST_CASE("fapo", "disabled valid pass-through", TestFapoDisabledValidPassThrough)
    NATIVE_TEST_CASE("fapo", "unsafe process buffers rejected", TestFapoRejectsUnsafeProcessBuffers)
    NATIVE_TEST_CASE("fapo", "silent input is never read", TestFapoSilentInputNeverReadsBuffer)
    NATIVE_TEST_CASE("fapo", "invalid parameter blocks rejected", TestFapoRejectsInvalidParameterBlocks)
    NATIVE_TEST_CASE("fapo", "GetParameters destination validated", TestFapoGetParametersValidatesDestination)
    NATIVE_TEST_CASE("fapo", "invalid lock formats rejected", TestFapoLockRejectsInvalidFormats)
    NATIVE_TEST_CASE("fapo", "silent input drains tail", TestFapoSilentInputDrainsTail)
    NATIVE_TEST_CASE("fapo", "valid zero blocks drain reported tail", TestFapoValidZeroBlocksDrainUntilReportedTailEnds)
    NATIVE_TEST_CASE("format", "explicit mappings", TestAudioOutputFormatMappings)
    NATIVE_TEST_CASE("format", "bit-perfect handshake requires exact packed PCM", TestBitPerfectFormatHandshakeRequiresExactPackedPcm)
    NATIVE_TEST_CASE("format", "raw metadata mappings", TestRawAudioFormatInfoMappings)
    NATIVE_TEST_CASE("format", "source bitrate calculation", TestAudioSourceBitrateCalculation)
    NATIVE_TEST_CASE("format", "average bitrate quality flags", TestAverageAudioQualityClassification)
    NATIVE_TEST_CASE("format", "explicit PCM24 mapping and frame bytes", TestExplicit24BitAudioOutputFormatMapping)
    NATIVE_TEST_CASE("format", "System device mapping", TestSystemAudioOutputFormatMapping)
    NATIVE_TEST_CASE("format", "System PCM24 mapping and frame bytes", TestSystem24BitPcmOutputFormatMapping)
    NATIVE_TEST_CASE("format", "aresample accepts mappings", TestAresampleAcceptsOutputFormats)
    NATIVE_TEST_CASE("fft", "fixed 48 kHz S16 stereo resampling", TestFftResamplesToLegacyFormat)
    NATIVE_TEST_CASE("fft", "PCM24 FIFO and resampling frame bytes", Test24BitFifoAndFftFrameSizing)
    NATIVE_TEST_CASE("fft", "surround resampling", TestFftSurroundResampleMatchesSwresample)
    NATIVE_TEST_CASE("fft", "legacy core behavior", TestFftLegacyCoreBehavior)
	NATIVE_TEST_CASE("fft", "presentation timeline ignores prefetch depth", TestFftTimelineFollowsPresentationCursor)
	NATIVE_TEST_CASE("fft", "generation is published only after timeline reset", TestFftObserverPublishesGenerationAfterReset)
    NATIVE_TEST_CASE("buffering", "CPU profiles favor normalized FIFO", TestAudioPipelineBufferingProfiles)
    NATIVE_TEST_CASE("audio pipeline", "normalized PCM precedes sink effects", TestAudioPipelineRoutesNormalizedPcmBeforeSinkEffects)
    NATIVE_TEST_CASE("audio pipeline", "stale generations rejected", TestAudioPipelineRejectsStaleGenerations)
    NATIVE_TEST_CASE("audio pipeline", "empty stream gets explicit EOS", TestAudioSinkCreatesExplicitEosForEmptyStream)
    NATIVE_TEST_CASE("audio pipeline", "shared device keeps sink EOS independent", TestAudioSinksShareDeviceButKeepEosStateIndependent)
    NATIVE_TEST_CASE("audio pipeline", "flat EQ skips FAPO and completes EOS", TestFAudioSinkSkipsFlatEqualizerAndCompletesEos)
    NATIVE_TEST_CASE("audio pipeline", "real FAudio sink drains tail before EOS", TestFAudioSinkDrainsTailThenCompletesActualEos)
	NATIVE_TEST_CASE("wasapi clock", "normal 48 kHz rate", TestWasapiClockHealthAcceptsNormalRate)
	NATIVE_TEST_CASE("wasapi clock", "short jitter is ignored", TestWasapiClockHealthIgnoresShortJitter)
	NATIVE_TEST_CASE("wasapi clock", "normal 44.1 kHz rate", TestWasapiClockHealthSupports44100Hz)
	NATIVE_TEST_CASE("wasapi clock", "two slow windows request recovery", TestWasapiClockHealthRequiresTwoSlowWindows)
	NATIVE_TEST_CASE("wasapi clock", "reset clears slow history", TestWasapiClockHealthResetClearsSlowHistory)
	NATIVE_TEST_CASE("wasapi format", "endpoint mix conversion", TestWasapiMixFormatConversion)
	NATIVE_TEST_CASE("wasapi format", "explicit requests retain fallbacks and wire variants", TestWasapiExclusiveProbeFallbacksAndWireVariants)
	NATIVE_TEST_CASE("wasapi format", "signed PCM32 conversion", TestPcm32IntegerConversion)
    NATIVE_TEST_CASE("lrc parser", "timestamp fractions normalize", TestLrcTimestampFractionNormalization)
    NATIVE_TEST_CASE("lrc parser", "progress delimiters parse", TestLrcProgressDelimiterParsing)
    NATIVE_TEST_CASE("lrc parser", "multi-line plain text and controller selection", TestLrcProgressMultiLinePlainTextAndControllerSelection)

#undef NATIVE_TEST_CASE
#undef NATIVE_REQUIRE
}
