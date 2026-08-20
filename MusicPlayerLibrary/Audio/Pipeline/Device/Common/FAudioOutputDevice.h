// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <vector>

#include <FAudio.h>

#include "Audio/AudioOutputFormat.h"
#include "Audio/FAudioResource.h"

namespace MusicPlayerLibrary
{
	// Explicit devices may be cached, but the mastering voice geometry is also
	// determined by the requested source format. The dynamic default deliberately
	// returns an empty key so it is resolved afresh on every acquisition.
	[[nodiscard]] std::string MakeFAudioOutputDeviceCacheKey(
		const AudioOutputFormat& requested);

	class FAudioOutputDevice final
	{
		UniqueFAudio engine_;
		UniqueFAudioVoice mastering_voice_;
		AudioOutputFormat output_format_{};
		FAudioWaveFormatExtensible system_format_{};
		AudioOutputDeviceInfo device_info_{};
		std::uint32_t device_index_ = 0;

		explicit FAudioOutputDevice(const AudioOutputFormat& requested);

	public:
		FAudioOutputDevice(const FAudioOutputDevice&) = delete;
		FAudioOutputDevice& operator=(const FAudioOutputDevice&) = delete;
		~FAudioOutputDevice();

		[[nodiscard]] static std::shared_ptr<FAudioOutputDevice> Acquire(
			const AudioOutputFormat& requested = {});
		[[nodiscard]] static std::vector<AudioOutputDeviceInfo>
			EnumerateOutputDevices();
		// Releases the application-lifetime cache. Existing sinks retain their
		// shared reference until their source voices have been destroyed.
		static void ShutdownShared() noexcept;
		// Runtime invalidation keeps the cache usable for a later backend rebuild.
		static void InvalidateShared() noexcept;
		[[nodiscard]] FAudio* GetEngine() const noexcept { return engine_.get(); }
		[[nodiscard]] const AudioOutputDeviceInfo& GetDeviceInfo() const noexcept
		{
			return device_info_;
		}
		[[nodiscard]] const std::string& GetDeviceId() const noexcept
		{
			return device_info_.id;
		}
		[[nodiscard]] std::uint32_t GetDeviceIndex() const noexcept
		{
			return device_index_;
		}
		[[nodiscard]] const AudioOutputFormat& GetOutputFormat() const noexcept
		{
			return output_format_;
		}
		[[nodiscard]] AudioOutputFormat ResolveSinkFormat(
			const AudioOutputFormat& requested) const;
		[[nodiscard]] std::uint32_t GetCurrentLatencyInSamples() const noexcept;
		void SetMasterVolume(float volume) noexcept;
	};
}
