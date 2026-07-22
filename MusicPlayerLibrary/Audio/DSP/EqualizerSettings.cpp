// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/DSP/EqualizerSettings.h"

#include <algorithm>

namespace MusicPlayerLibrary::AudioDsp
{
	int EqualizerSettings::GetBand(const int index) const noexcept
	{
		if (index < 0 || static_cast<std::size_t>(index) >= EqualizerBandCount)
			return 0;
		return static_cast<int>(config_.bands[index].gain_db);
	}

	bool EqualizerSettings::SetBand(const int index, int value) noexcept
	{
		if (index < 0 || static_cast<std::size_t>(index) >= EqualizerBandCount)
			return false;
		value = std::clamp(
			value, MinimumEqualizerBandGainDb, MaximumEqualizerBandGainDb);
		auto& gain_db = config_.bands[index].gain_db;
		if (gain_db == static_cast<float>(value))
			return false;
		gain_db = static_cast<float>(value);
		return true;
	}

	EqualizerPublication EqualizerSettings::PreparePublication(
		const std::uint32_t sample_rate,
		const bool force_reset,
		const bool currently_enabled,
		const bool additional_processing_required) noexcept
	{
		auto snapshot = Compile(sample_rate);
		const bool should_enable = snapshot.enabled_mask != 0 ||
			additional_processing_required;
		if (force_reset || should_enable != currently_enabled)
		{
			AdvanceResetGeneration();
			snapshot = Compile(sample_rate);
		}
		return {snapshot, should_enable};
	}
}
