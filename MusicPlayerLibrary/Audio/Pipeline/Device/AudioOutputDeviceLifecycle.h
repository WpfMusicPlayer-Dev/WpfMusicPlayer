// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
	// Releases cached strong references without permanently shutting the caches
	// down. Existing sinks remain alive; later Acquire calls create fresh devices.
	void InvalidateAudioOutputDevices() noexcept;
	void ShutdownAudioOutputDevices() noexcept;
}
