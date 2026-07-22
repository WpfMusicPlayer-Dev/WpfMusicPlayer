// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace MusicPlayerLibrary
{
	template <typename Device>
	class SharedAudioDeviceCache final
	{
		std::mutex mutex_;
		std::unordered_map<std::string, std::shared_ptr<Device>> devices_;
		bool shutdown_ = false;
		const char* shutdown_message_;

	public:
		explicit SharedAudioDeviceCache(const char* shutdown_message) noexcept :
			shutdown_message_(shutdown_message)
		{
		}

		SharedAudioDeviceCache(const SharedAudioDeviceCache&) = delete;
		SharedAudioDeviceCache& operator=(const SharedAudioDeviceCache&) = delete;

		template <typename Factory, typename CachePredicate>
		[[nodiscard]] std::shared_ptr<Device> Acquire(
			const std::string& device_id,
			Factory&& factory,
			CachePredicate&& cache_predicate)
		{
			std::lock_guard lock(mutex_);
			if (shutdown_)
				throw std::logic_error(shutdown_message_);

			// An empty identifier follows the backend's current default device and
			// must be resolved afresh on every acquisition.
			if (device_id.empty())
				return std::forward<Factory>(factory)();

			if (const auto existing = devices_.find(device_id);
				existing != devices_.end())
			{
				return existing->second;
			}

			auto device = std::forward<Factory>(factory)();
			if (std::forward<CachePredicate>(cache_predicate)(*device))
				devices_.emplace(device_id, device);
			return device;
		}

		void Shutdown() noexcept
		{
			decltype(devices_) released;
			{
				std::lock_guard lock(mutex_);
				shutdown_ = true;
				released.swap(devices_);
			}
			released.clear();
		}
	};
}
