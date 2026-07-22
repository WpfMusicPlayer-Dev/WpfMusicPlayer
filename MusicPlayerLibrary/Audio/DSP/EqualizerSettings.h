// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "Audio/DSP/EqualizerDsp.h"

namespace MusicPlayerLibrary::AudioDsp
{
	struct EqualizerPublication
	{
		EqualizerDspSnapshot snapshot{};
		bool processing_enabled = false;
	};

	// Backend-neutral user controls and DSP reset generation. Callers provide
	// their own synchronization and publish the resulting snapshot themselves.
	class EqualizerSettings final
	{
		EqualizerConfig config_ = MakeDefaultTenBandConfig();
		std::uint64_t reset_generation_ = 0;

	public:
		[[nodiscard]] int GetBand(int index) const noexcept;
		[[nodiscard]] bool SetBand(int index, int value) noexcept;

		void AdvanceResetGeneration() noexcept { ++reset_generation_; }

		[[nodiscard]] EqualizerDspSnapshot Compile(
			std::uint32_t sample_rate,
			float pre_gain = 1.0f) const noexcept
		{
			return CompileEqualizerSnapshot(
				config_, sample_rate, reset_generation_, pre_gain);
		}

		[[nodiscard]] EqualizerPublication PreparePublication(
			std::uint32_t sample_rate,
			bool force_reset,
			bool currently_enabled,
			bool additional_processing_required = false) noexcept;
	};
}
