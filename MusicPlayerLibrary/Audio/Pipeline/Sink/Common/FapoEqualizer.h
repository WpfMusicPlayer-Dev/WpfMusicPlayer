// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

#include "Audio/DSP/EqualizerDsp.h"
#include <FAPO.h>

namespace MusicPlayerLibrary::AudioDsp
{
	struct FapoDeleter final
	{
		void operator()(FAPO* effect) const noexcept
		{
			if (effect != nullptr)
				effect->Release(effect);
		}
	};

	using UniqueFapo = std::unique_ptr<FAPO, FapoDeleter>;

    [[nodiscard]] std::uint32_t CreateEqualizerFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter,
        FAPO** effect) noexcept;

    // These helpers publish only atomics written after a successful Process
    // pass; they never read EqualizerDsp concurrently with the audio thread.
    [[nodiscard]] bool EqualizerFapoHasTail(FAPO* effect) noexcept;
    [[nodiscard]] std::uint64_t EqualizerFapoProcessSequence(
        FAPO* effect) noexcept;
}
