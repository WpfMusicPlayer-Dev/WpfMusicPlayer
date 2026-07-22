// SPDX-License-Identifier: MIT

#pragma once

#include <memory>

#include "Audio/AudioOutputFormat.h"

namespace MusicPlayerLibrary
{
	class IAudioSink;

	[[nodiscard]] std::shared_ptr<IAudioSink> CreateAudioSink(
		const AudioOutputFormat& requested);
}
