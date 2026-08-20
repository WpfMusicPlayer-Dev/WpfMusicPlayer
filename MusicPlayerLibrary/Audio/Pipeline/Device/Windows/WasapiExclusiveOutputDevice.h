// SPDX-License-Identifier: MIT

#pragma once

#if defined(_WIN32)

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Audio/AudioOutputFormat.h"

struct IAudioClient;

namespace MusicPlayerLibrary
{
	class WasapiExclusiveOutputDevice final
	{
		AudioOutputFormat output_format_{};
		AudioOutputDeviceInfo device_info_{};
		std::wstring endpoint_id_;
		std::int64_t default_device_period_100ns_ = 0;
		std::int64_t minimum_device_period_100ns_ = 0;

		explicit WasapiExclusiveOutputDevice(const AudioOutputFormat& requested);

	public:
		WasapiExclusiveOutputDevice(const WasapiExclusiveOutputDevice&) = delete;
		WasapiExclusiveOutputDevice& operator=(
			const WasapiExclusiveOutputDevice&) = delete;
		~WasapiExclusiveOutputDevice();

		[[nodiscard]] static std::shared_ptr<WasapiExclusiveOutputDevice> Acquire(
			const AudioOutputFormat& requested = {});
		[[nodiscard]] static std::vector<AudioOutputDeviceInfo>
			EnumerateOutputDevices();
		static void ShutdownShared() noexcept;
		static void InvalidateShared() noexcept;

		[[nodiscard]] const AudioOutputFormat& GetOutputFormat() const noexcept
		{
			return output_format_;
		}
		[[nodiscard]] const AudioOutputDeviceInfo& GetDeviceInfo() const noexcept
		{
			return device_info_;
		}
		[[nodiscard]] const std::string& GetDeviceId() const noexcept
		{
			return device_info_.id;
		}
		[[nodiscard]] AudioOutputFormat ResolveSinkFormat(
			const AudioOutputFormat& requested) const;

		// Returns an AddRef-owned client for this endpoint. The caller must have
		// initialized COM on the calling thread and must Release the result.
		[[nodiscard]] IAudioClient* ActivateAudioClient() const;
		[[nodiscard]] std::int64_t
			GetDefaultDevicePeriod100Nanoseconds() const noexcept
		{
			return default_device_period_100ns_;
		}
		[[nodiscard]] std::int64_t
			GetMinimumDevicePeriod100Nanoseconds() const noexcept
		{
			return minimum_device_period_100ns_;
		}
		[[nodiscard]] std::uint32_t GetCurrentLatencyInSamples() const noexcept;
	};
}

#endif
