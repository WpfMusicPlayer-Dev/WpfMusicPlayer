// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <numbers>

#include "Audio/FFT/AudioPipelinePerformanceHelper.h"
#include "Audio/FFT/KissFftConfig.h"

namespace
{
	constexpr int SmallFftSize = 256;
	constexpr int WarmupIterations = 8;
	constexpr int MeasuredIterations = 48;
	constexpr int MeasurementBatches = 5;

	double BenchmarkSmallFftMicroseconds() noexcept
	{
		try
		{
			auto config = MusicPlayerLibrary::AllocateKissFftConfig(SmallFftSize);
			if (!config)
				return 0.0;

			std::array<kiss_fft_cpx, SmallFftSize> input{};
			std::array<kiss_fft_cpx, SmallFftSize> output{};
			for (int index = 0; index < SmallFftSize; ++index)
			{
				input[index].r = std::sin(
					2.0 * std::numbers::pi * static_cast<double>(index) / SmallFftSize);
			}

			for (int iteration = 0; iteration < WarmupIterations; ++iteration)
				kiss_fft(config.get(), input.data(), output.data());

			std::array<double, MeasurementBatches> batch_costs{};
			for (double& batch_cost : batch_costs)
			{
				const auto begin = std::chrono::steady_clock::now();
				for (int iteration = 0; iteration < MeasuredIterations; ++iteration)
					kiss_fft(config.get(), input.data(), output.data());
				const auto elapsed = std::chrono::steady_clock::now() - begin;
				batch_cost = std::chrono::duration<double, std::micro>(elapsed).count()
					/ MeasuredIterations;
			}
			std::ranges::sort(batch_costs);
			const double median_cost = batch_costs[MeasurementBatches / 2];
			return std::isfinite(median_cost) && median_cost > 0.0
				? median_cost
				: 0.0;
		}
		catch (...)
		{
			return 0.0;
		}
	}
}

MusicPlayerLibrary::AudioPipelineBufferingProfile
MusicPlayerLibrary::SelectAudioPipelineBufferingProfile(
	const double small_fft_microseconds) noexcept
{
	AudioPipelineBufferingProfile profile{};
	profile.small_fft_microseconds = small_fft_microseconds;
	if (std::isfinite(small_fft_microseconds) && small_fft_microseconds > 0.0)
	{
		if (small_fft_microseconds <= 15.0)
		{
			profile.fifo_target_milliseconds = 80;
			profile.decoded_queue_target_milliseconds = 24;
		}
		else if (small_fft_microseconds > 50.0)
		{
			profile.fifo_target_milliseconds = 220;
			profile.decoded_queue_target_milliseconds = 48;
		}
	}
	
	NATIVE_TRACE(
		"info: native prewarm CPU estimate: 256-point FFT %.2f us, fifo target %d ms, decoded queue target %d ms\n",
		profile.small_fft_microseconds,
		profile.fifo_target_milliseconds,
		profile.decoded_queue_target_milliseconds);
	return profile;
}

const MusicPlayerLibrary::AudioPipelineBufferingProfile&
MusicPlayerLibrary::GetAudioPipelineBufferingProfile() noexcept
{
	static const AudioPipelineBufferingProfile profile =
		SelectAudioPipelineBufferingProfile(BenchmarkSmallFftMicroseconds());
	return profile;
}

void MusicPlayerLibrary::PrecomputeAudioPipelineBufferingProfile() noexcept
{
	(void)GetAudioPipelineBufferingProfile();
}
