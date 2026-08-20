// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/Pipeline/Sink/AudioSinkFactory.h"

#include <stdexcept>

#include "Audio/Pipeline/Sink/Common/FAudioSink.h"
#if defined(_WIN32)
#include "Audio/Pipeline/Sink/Windows/WasapiExclusiveSink.h"
#endif

namespace MusicPlayerLibrary
{
	std::shared_ptr<IAudioSink> CreateAudioSink(
		const AudioOutputFormat& requested)
	{
		switch (requested.requested_backend)
		{
		case AudioBackend::FAudio:
			return std::make_shared<FAudioSink>(requested);
#if defined(_WIN32)
		case AudioBackend::WasapiExclusive:
			try
			{
				return std::make_shared<WasapiExclusiveSink>(requested);
			}
			catch (const std::exception& exception)
			{
				NATIVE_TRACE(
					"warn: WASAPI exclusive initialization failed; "
					"falling back to shared FAudio: %s\n",
					exception.what());
				// WASAPI format discovery intentionally ignores these configured
				// values, but shared FAudio still owns their fallback semantics.
				AudioOutputFormat fallback = requested;
				fallback.requested_backend = AudioBackend::FAudio;
				return std::make_shared<FAudioSink>(fallback);
			}
#endif
		default:
			throw std::invalid_argument("Unsupported audio backend");
		}
	}
}
