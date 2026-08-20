// SPDX-License-Identifier: MIT

#pragma once

#if defined(_WIN32)

#include <cstdint>
#include <memory>

namespace MusicPlayerLibrary
{
	class IAudioOutputDeviceChangeSink
	{
	public:
		virtual ~IAudioOutputDeviceChangeSink() = default;
		// Called from a system notification thread. Implementations must return
		// promptly and marshal any UI or pipeline work to an owning thread.
		virtual void OnAudioOutputDeviceChanged() noexcept = 0;
	};

	class AudioOutputDeviceChangeSubscription final
	{
		std::uint64_t subscription_id_ = 0;

		explicit AudioOutputDeviceChangeSubscription(
			std::uint64_t subscription_id) noexcept;
		friend std::unique_ptr<AudioOutputDeviceChangeSubscription>
			SubscribeAudioOutputDeviceChanges(IAudioOutputDeviceChangeSink* sink);

	public:
		AudioOutputDeviceChangeSubscription(
			const AudioOutputDeviceChangeSubscription&) = delete;
		AudioOutputDeviceChangeSubscription& operator=(
			const AudioOutputDeviceChangeSubscription&) = delete;
		~AudioOutputDeviceChangeSubscription();
	};

	[[nodiscard]] std::unique_ptr<AudioOutputDeviceChangeSubscription>
		SubscribeAudioOutputDeviceChanges(IAudioOutputDeviceChangeSink* sink);
	[[nodiscard]] std::uint64_t GetAudioOutputDeviceChangeRevision() noexcept;
	void ShutdownAudioOutputDeviceNotifications() noexcept;
}

#endif // defined(_WIN32)
