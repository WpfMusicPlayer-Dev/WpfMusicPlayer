// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <FAudio.h>

namespace MusicPlayerLibrary
{
	struct FAudioDeleter final
	{
		void operator()(FAudio* engine) const noexcept
		{
			if (engine != nullptr)
				FAudio_Release(engine);
		}
	};

	using UniqueFAudio = std::unique_ptr<FAudio, FAudioDeleter>;

	struct FAudioVoiceDeleter final
	{
		void operator()(FAudioVoice* voice) const noexcept
		{
			if (voice != nullptr)
				FAudioVoice_DestroyVoice(voice);
		}
	};

	using UniqueFAudioVoice = std::unique_ptr<FAudioVoice, FAudioVoiceDeleter>;

	[[nodiscard]] inline UniqueFAudio TryCreateFAudioEngine()
	{
		FAudio* raw_engine = nullptr;
		const std::uint32_t result = FAudioCreate(
			&raw_engine, 0, FAUDIO_DEFAULT_PROCESSOR);
		UniqueFAudio engine(raw_engine);
		return result == FAUDIO_OK ? std::move(engine) : UniqueFAudio{};
	}

	[[nodiscard]] inline UniqueFAudio CreateFAudioEngine(
		const char* failure_message)
	{
		auto engine = TryCreateFAudioEngine();
		if (!engine)
			throw std::runtime_error(failure_message);
		return engine;
	}
}
