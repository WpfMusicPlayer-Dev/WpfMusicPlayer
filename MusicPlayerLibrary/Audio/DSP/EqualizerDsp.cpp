// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Audio/DSP/EqualizerDsp.h"

#include <algorithm>
#include <limits>
#include <numbers>

namespace MusicPlayerLibrary::AudioDsp
{
    namespace
    {
        constexpr std::uint32_t MaxChannelCount = 64;
        constexpr float TailThreshold = 1.0e-8f;
		constexpr float LoudnessPowerFloor = 1.0e-30f;
		// All ten public bands can move by +/-24 dB. Their theoretical summed
		// bound is +/-240 dB, or 1e-12..1e12 in linear gain. The final limiter
		// and anti-windup remain the full-scale safety boundary.
		constexpr float MinimumLoudnessMatchGain = 1.0e-12f;
		constexpr float MaximumLoudnessMatchGain = 1.0e12f;
        constexpr double LoudnessPowerWindowSeconds = 0.100;
        constexpr double LoudnessGainWindowSeconds = 0.050;
		constexpr double LoudnessFeedbackWarmupSeconds = 0.250;

        [[nodiscard]] bool IsUsableBand(
            const EqualizerBandConfig& band, std::uint32_t sampleRate) noexcept
        {
            return band.enabled &&
                band.type == BiquadType::Peaking &&
                band.gain_db != 0.0f &&
                std::isfinite(band.gain_db) &&
                std::isfinite(band.frequency_hz) &&
                band.frequency_hz > 0.0f &&
                std::isfinite(band.q) &&
                band.q > 0.0f &&
                sampleRate != 0 &&
                2.0 * static_cast<double>(band.frequency_hz) < sampleRate;
        }

        [[nodiscard]] bool TryCompilePeaking(
            const EqualizerBandConfig& band,
            std::uint32_t sampleRate,
            BiquadCoefficients& coefficients) noexcept
        {
            if (!IsUsableBand(band, sampleRate))
                return false;

            const double frequency = band.frequency_hz;
            const double q = band.q;
            const double gainDb = band.gain_db;
            const double a = std::pow(10.0, gainDb / 40.0);
            const double omega = 2.0 * std::numbers::pi_v<double> *
                frequency / static_cast<double>(sampleRate);
            const double alpha = std::sin(omega) / (2.0 * q);
            const double cosine = std::cos(omega);
            const double a0 = 1.0 + alpha / a;

            const double normalizedB0 = (1.0 + alpha * a) / a0;
            const double normalizedB1 = (-2.0 * cosine) / a0;
            const double normalizedB2 = (1.0 - alpha * a) / a0;
            const double normalizedA1 = (-2.0 * cosine) / a0;
            const double normalizedA2 = (1.0 - alpha / a) / a0;

            if (!std::isfinite(normalizedB0) ||
                !std::isfinite(normalizedB1) ||
                !std::isfinite(normalizedB2) ||
                !std::isfinite(normalizedA1) ||
                !std::isfinite(normalizedA2))
            {
                return false;
            }

            coefficients.b0 = static_cast<float>(normalizedB0);
            coefficients.b1 = static_cast<float>(normalizedB1);
            coefficients.b2 = static_cast<float>(normalizedB2);
            coefficients.a1 = static_cast<float>(normalizedA1);
            coefficients.a2 = static_cast<float>(normalizedA2);
            return std::isfinite(coefficients.b0) &&
                std::isfinite(coefficients.b1) &&
                std::isfinite(coefficients.b2) &&
                std::isfinite(coefficients.a1) &&
                std::isfinite(coefficients.a2);
        }
    }

    EqualizerConfig MakeDefaultTenBandConfig() noexcept
    {
        EqualizerConfig config;
        for (std::size_t index = 0; index < EqualizerBandCount; ++index)
        {
            config.bands[index].type = BiquadType::Peaking;
            config.bands[index].frequency_hz = EqualizerBandFrequenciesHz[index];
            config.bands[index].q = 1.0f;
            config.bands[index].gain_db = 0.0f;
            config.bands[index].enabled = true;
        }
        return config;
    }

	LimiterConfig MakeSinkLimiterConfig() noexcept
	{
		return {
			.enabled = true,
			.ceiling = 1.0f,
			.lookahead_ms = 0.0f,
			.release_ms = 50.0f,
			.match_input_loudness = true
		};
	}

	bool IsValidEqualizerSnapshotHeader(
		const EqualizerDspSnapshot& snapshot) noexcept
	{
		return snapshot.abi_version == EqualizerSnapshotAbiVersion &&
			snapshot.byte_size == sizeof(EqualizerDspSnapshot) &&
			std::isfinite(snapshot.pre_gain) &&
			snapshot.pre_gain >= MinimumEqualizerPreGain &&
			snapshot.pre_gain <= MaximumEqualizerPreGain;
	}

    EqualizerDspSnapshot CompileEqualizerSnapshot(
        const EqualizerConfig& config,
        std::uint32_t sampleRate,
        std::uint64_t resetGeneration,
        const float preGain) noexcept
    {
        EqualizerDspSnapshot snapshot;
        snapshot.byte_size = sizeof(EqualizerDspSnapshot);
        snapshot.reset_generation = resetGeneration;
        snapshot.pre_gain = std::isfinite(preGain)
            ? std::clamp(
				preGain, MinimumEqualizerPreGain, MaximumEqualizerPreGain)
            : 1.0f;

        for (std::size_t index = 0; index < EqualizerBandCount; ++index)
        {
            BiquadCoefficients coefficients;
            if (!TryCompilePeaking(config.bands[index], sampleRate, coefficients))
                continue;

            snapshot.bands[index] = coefficients;
            snapshot.enabled_mask |= std::uint32_t{1} << index;
        }

        return snapshot;
    }

    bool EqualizerDsp::Prepare(
        std::uint32_t sampleRate,
        std::uint32_t channelCount,
        std::uint32_t maxFrameCount,
        const LimiterConfig& limiter)
    {
        if (sampleRate == 0 || channelCount == 0 ||
            channelCount > MaxChannelCount || maxFrameCount == 0 ||
            !std::isfinite(limiter.ceiling) || limiter.ceiling <= 0.0f ||
            !std::isfinite(limiter.lookahead_ms) || limiter.lookahead_ms < 0.0f ||
            !std::isfinite(limiter.release_ms) || limiter.release_ms <= 0.0f)
        {
            return false;
        }

        const double lookaheadFrames = std::floor(
            static_cast<double>(sampleRate) * limiter.lookahead_ms / 1000.0);
        const double releaseFrames = std::floor(
            static_cast<double>(sampleRate) * limiter.release_ms / 1000.0);
        if (lookaheadFrames > (std::numeric_limits<std::uint32_t>::max)() ||
            releaseFrames > (std::numeric_limits<std::uint32_t>::max)())
        {
            return false;
        }

        sample_rate_ = sampleRate;
        channel_count_ = channelCount;
        max_frame_count_ = maxFrameCount;
        limiter_ = limiter;
        lookahead_window_frames_ = std::max(
            1u, static_cast<std::uint32_t>(lookaheadFrames));
        limiter_delay_frames_ = limiter.enabled ? lookahead_window_frames_ - 1u : 0u;
        limiter_release_frames_ = std::max(
            1u, static_cast<std::uint32_t>(releaseFrames));
        loudness_power_history_ = static_cast<float>(std::exp(
            -1.0 / (static_cast<double>(sampleRate) *
                LoudnessPowerWindowSeconds)));
        loudness_gain_history_ = static_cast<float>(std::exp(
            -1.0 / (static_cast<double>(sampleRate) *
                LoudnessGainWindowSeconds)));
        biquad_states_.assign(
            static_cast<std::size_t>(channelCount) * EqualizerBandCount, {});
        delay_line_.assign(
            static_cast<std::size_t>(lookahead_window_frames_) * channelCount, 0.0f);
		input_power_delay_line_.assign(lookahead_window_frames_, 0.0f);
        peak_queue_.assign(
            static_cast<std::size_t>(lookahead_window_frames_) + 1u, {});
        Reset();
        return true;
    }

    void EqualizerDsp::Reset() noexcept
    {
        std::fill(biquad_states_.begin(), biquad_states_.end(), BiquadState{});
        std::fill(delay_line_.begin(), delay_line_.end(), 0.0f);
		std::fill(
			input_power_delay_line_.begin(), input_power_delay_line_.end(), 0.0f);
        std::fill(peak_queue_.begin(), peak_queue_.end(), PeakNode{});
        limiter_frame_index_ = 0;
        silent_input_frames_ = 0;
        delay_write_frame_ = 0;
        peak_head_ = 0;
        peak_tail_ = 0;
        limiter_gain_ = 1.0f;
        limiter_release_step_ = 0.0f;
        limiter_release_frames_remaining_ = 0;
        input_loudness_power_ = 0.0f;
        equalized_loudness_power_ = 0.0f;
		delayed_input_loudness_power_ = 0.0f;
		output_loudness_power_ = 0.0f;
        loudness_match_gain_ = 1.0f;
		loudness_measurement_frames_ = 0;
        has_tail_ = false;
        applied_reset_generation_ = ~std::uint64_t{0};
    }

	void EqualizerDsp::UpdateLoudnessMatchGain(
		const float target_gain) noexcept
	{
		loudness_match_gain_ =
			loudness_gain_history_ * loudness_match_gain_ +
			(1.0f - loudness_gain_history_) * target_gain;
	}

    bool EqualizerDsp::Process(
        const EqualizerDspSnapshot& snapshot,
        const float* input,
        float* output,
        std::uint32_t frameCount,
        bool inputSilent) noexcept
    {
        if (sample_rate_ == 0 || channel_count_ == 0 ||
            frameCount > max_frame_count_ ||
            (frameCount != 0 &&
             (output == nullptr || (!inputSilent && input == nullptr))) ||
			!IsValidEqualizerSnapshotHeader(snapshot))
        {
            return false;
        }

        if (applied_reset_generation_ != snapshot.reset_generation)
        {
            Reset();
            applied_reset_generation_ = snapshot.reset_generation;
        }

        for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
        {
            for (std::size_t band = 0; band < EqualizerBandCount; ++band)
            {
                if ((snapshot.enabled_mask & (std::uint32_t{1} << band)) == 0)
                {
                    biquad_states_[
                        static_cast<std::size_t>(channel) * EqualizerBandCount + band] = {};
                }
            }
        }

        bool outputActive = false;
        std::array<float, MaxChannelCount> filteredSamples{};
        for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        {
            float inputSquareSum = 0.0f;
            float equalizedSquareSum = 0.0f;
            for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
            {
                const auto sampleIndex =
                    static_cast<std::size_t>(frame) * channel_count_ + channel;
                const float inputSample = inputSilent
                    ? 0.0f
                    : input[sampleIndex];
                float x = inputSample * snapshot.pre_gain;

                for (std::size_t band = 0; band < EqualizerBandCount; ++band)
                {
                    if ((snapshot.enabled_mask & (std::uint32_t{1} << band)) == 0)
                        continue;

                    const auto& coefficients = snapshot.bands[band];
                    auto& state = biquad_states_[
                        static_cast<std::size_t>(channel) * EqualizerBandCount + band];
                    const float y = coefficients.b0 * x + state.z1;
                    state.z1 = coefficients.b1 * x - coefficients.a1 * y + state.z2;
                    state.z2 = coefficients.b2 * x - coefficients.a2 * y;
                    x = y;
                }

                filteredSamples[channel] = x;
                inputSquareSum += inputSample * inputSample;
                equalizedSquareSum += x * x;
            }

            if (limiter_.match_input_loudness && !inputSilent)
            {
                const float inverseChannelCount =
                    1.0f / static_cast<float>(channel_count_);
                const float inputPower = inputSquareSum * inverseChannelCount;
                const float equalizedPower =
                    equalizedSquareSum * inverseChannelCount;
                const float currentPowerWeight = 1.0f - loudness_power_history_;
                input_loudness_power_ =
                    loudness_power_history_ * input_loudness_power_ +
                    currentPowerWeight * inputPower;
                equalized_loudness_power_ =
                    loudness_power_history_ * equalized_loudness_power_ +
                    currentPowerWeight * equalizedPower;

                if (input_loudness_power_ >= LoudnessPowerFloor &&
                    equalized_loudness_power_ >= LoudnessPowerFloor)
                {
                    const float targetGain = std::clamp(
                        std::sqrt(input_loudness_power_ /
                            equalized_loudness_power_),
                        MinimumLoudnessMatchGain,
                        MaximumLoudnessMatchGain);
					UpdateLoudnessMatchGain(targetGain);
                }
            }
            else if (!limiter_.match_input_loudness)
            {
                loudness_match_gain_ = 1.0f;
            }

            float linkedPeak = 0.0f;
            for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
            {
                filteredSamples[channel] *= loudness_match_gain_;
                linkedPeak = std::max(
                    linkedPeak, std::abs(filteredSamples[channel]));
            }
			const float inverseChannelCount =
				1.0f / static_cast<float>(channel_count_);
			const float currentInputPower =
				inputSquareSum * inverseChannelCount;
			float loudnessReferencePower = currentInputPower;
			float finalOutputSquareSum = 0.0f;

            if (!limiter_.enabled)
            {
                for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
                {
                    const auto sampleIndex =
                        static_cast<std::size_t>(frame) * channel_count_ + channel;
                    output[sampleIndex] = filteredSamples[channel];
					finalOutputSquareSum +=
						filteredSamples[channel] * filteredSamples[channel];
                    outputActive = outputActive ||
                        std::abs(filteredSamples[channel]) >= TailThreshold;
                }
            }
            else
            {
                const std::size_t queueCapacity = peak_queue_.size();
                while (peak_head_ != peak_tail_ &&
                       limiter_frame_index_ - peak_queue_[peak_head_].frame_index >=
                           lookahead_window_frames_)
                {
                    peak_head_ = (peak_head_ + 1u) % queueCapacity;
                }

                while (peak_head_ != peak_tail_)
                {
                    const std::size_t last =
                        (peak_tail_ + queueCapacity - 1u) % queueCapacity;
                    if (peak_queue_[last].peak > linkedPeak)
                        break;
                    peak_tail_ = last;
                }
                peak_queue_[peak_tail_] = {limiter_frame_index_, linkedPeak};
                peak_tail_ = (peak_tail_ + 1u) % queueCapacity;

                const float windowPeak = peak_queue_[peak_head_].peak;
                const float targetGain = windowPeak > limiter_.ceiling
                    ? limiter_.ceiling / windowPeak
                    : 1.0f;
                if (targetGain < limiter_gain_)
                {
                    limiter_gain_ = targetGain;
                    limiter_release_step_ =
                        (1.0f - targetGain) / limiter_release_frames_;
                    limiter_release_frames_remaining_ = limiter_release_frames_;
                }
                else if (targetGain > limiter_gain_)
                {
                    limiter_gain_ = std::min(
                        targetGain, limiter_gain_ + limiter_release_step_);
                    if (limiter_release_frames_remaining_ != 0)
                    {
                        --limiter_release_frames_remaining_;
                        if (limiter_release_frames_remaining_ == 0)
                            limiter_gain_ = targetGain;
                    }
                }

                const std::size_t writeOffset = delay_write_frame_ * channel_count_;
                for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
                    delay_line_[writeOffset + channel] = filteredSamples[channel];
				input_power_delay_line_[delay_write_frame_] = currentInputPower;

                const std::size_t readFrame =
                    (delay_write_frame_ + lookahead_window_frames_ -
                     limiter_delay_frames_) % lookahead_window_frames_;
                const std::size_t readOffset = readFrame * channel_count_;
				loudnessReferencePower = input_power_delay_line_[readFrame];
				input_power_delay_line_[readFrame] = 0.0f;
                for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
                {
                    const auto sampleIndex =
                        static_cast<std::size_t>(frame) * channel_count_ + channel;
                    const float delayedSample = delay_line_[readOffset + channel];
                    delay_line_[readOffset + channel] = 0.0f;
                    const float limitedSample = (std::clamp)(
                        delayedSample * limiter_gain_,
                        -limiter_.ceiling,
                        limiter_.ceiling);
                    output[sampleIndex] = limitedSample;
					finalOutputSquareSum += limitedSample * limitedSample;
                    outputActive = outputActive ||
                        std::abs(limitedSample) >= TailThreshold;
                }

                delay_write_frame_ =
                    (delay_write_frame_ + 1u) % lookahead_window_frames_;
                ++limiter_frame_index_;
            }

			if (limiter_.match_input_loudness)
			{
				const auto feedbackWarmupFrames = static_cast<std::uint64_t>(
					static_cast<double>(sample_rate_) * LoudnessFeedbackWarmupSeconds);
				const auto feedbackMeasurementFrames = static_cast<std::uint64_t>(
					static_cast<double>(sample_rate_) * LoudnessPowerWindowSeconds);
				if (loudnessReferencePower >= LoudnessPowerFloor &&
					loudness_measurement_frames_ !=
						(std::numeric_limits<std::uint64_t>::max)())
				{
					++loudness_measurement_frames_;
				}
				if (loudness_measurement_frames_ == feedbackWarmupFrames)
				{
					delayed_input_loudness_power_ = 0.0f;
					output_loudness_power_ = 0.0f;
				}
				if (loudness_measurement_frames_ >= feedbackWarmupFrames)
				{
					const float currentPowerWeight = 1.0f - loudness_power_history_;
					delayed_input_loudness_power_ =
						loudness_power_history_ * delayed_input_loudness_power_ +
						currentPowerWeight * loudnessReferencePower;
					output_loudness_power_ =
						loudness_power_history_ * output_loudness_power_ +
						currentPowerWeight *
							(finalOutputSquareSum * inverseChannelCount);
				}

				if (loudness_measurement_frames_ >=
						feedbackWarmupFrames + feedbackMeasurementFrames &&
					delayed_input_loudness_power_ >= LoudnessPowerFloor &&
					output_loudness_power_ >= LoudnessPowerFloor)
				{
					const float correction = std::sqrt(
						delayed_input_loudness_power_ / output_loudness_power_);
					const bool limiterIsConstraining = limiter_.enabled &&
						limiter_gain_ < 1.0f - 1.0e-5f;
					// Reducing an overly loud output is always safe. Do not wind the
					// match gain upward when the peak limiter is already binding; an
					// exact RMS match is then impossible without exceeding full scale.
					if (correction <= 1.0f || !limiterIsConstraining)
					{
						const float targetGain = std::clamp(
							loudness_match_gain_ * correction,
							MinimumLoudnessMatchGain,
							MaximumLoudnessMatchGain);
						UpdateLoudnessMatchGain(targetGain);
					}
				}
			}

            if (inputSilent)
            {
                if (silent_input_frames_ !=
                    (std::numeric_limits<std::uint64_t>::max)())
                {
                    ++silent_input_frames_;
                }
            }
            else
            {
                silent_input_frames_ = 0;
            }
        }

        has_tail_ = false;
        for (const auto& state : biquad_states_)
        {
            if (std::abs(state.z1) >= TailThreshold ||
                std::abs(state.z2) >= TailThreshold)
            {
                has_tail_ = true;
                break;
            }
        }
        if (limiter_.enabled && !has_tail_)
        {
            has_tail_ = std::any_of(
                delay_line_.begin(), delay_line_.end(),
                [](float sample) { return std::abs(sample) >= TailThreshold; });
        }
        if (limiter_.enabled && !has_tail_ && peak_head_ != peak_tail_)
            has_tail_ = peak_queue_[peak_head_].peak >= TailThreshold;
        if (limiter_.enabled && !has_tail_)
            has_tail_ = std::abs(1.0f - limiter_gain_) >= TailThreshold;

        const bool fullSilentDelay = !limiter_.enabled ||
            silent_input_frames_ >= limiter_delay_frames_;
        if (inputSilent && fullSilentDelay && !outputActive && !has_tail_)
        {
            const auto resetGeneration = applied_reset_generation_;
            Reset();
            applied_reset_generation_ = resetGeneration;
        }

        return !inputSilent || outputActive || has_tail_;
    }

    bool EqualizerDsp::HasTail() const noexcept
    {
        return has_tail_;
    }

    std::uint32_t EqualizerDsp::GetLimiterDelayFrames() const noexcept
    {
        return limiter_delay_frames_;
    }
}
