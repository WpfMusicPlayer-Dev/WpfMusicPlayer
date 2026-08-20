// SPDX-License-Identifier: MIT

#include "pch.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Mmdeviceapi.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

#include "Audio/Pipeline/Device/Windows/AudioOutputDeviceNotification.h"
#include "Audio/Pipeline/Device/Windows/WasapiExclusiveOutputDevice.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WindowsResource.h"

namespace
{
	using MusicPlayerLibrary::IAudioOutputDeviceChangeSink;

	class AudioOutputDeviceNotificationService;

	class EndpointNotificationClient final : public IMMNotificationClient
	{
		std::atomic<ULONG> reference_count_{1};
		std::atomic<AudioOutputDeviceNotificationService*> owner_;

		void PublishChange() noexcept;

	public:
		explicit EndpointNotificationClient(
			AudioOutputDeviceNotificationService* owner) noexcept :
			owner_(owner)
		{
		}

		void Detach() noexcept
		{
			owner_.store(nullptr, std::memory_order_release);
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(
			REFIID interface_id,
			void** object) noexcept override
		{
			if (!object)
				return E_POINTER;
			*object = nullptr;
			if (InlineIsEqualGUID(interface_id, __uuidof(IUnknown)) ||
				InlineIsEqualGUID(interface_id, __uuidof(IMMNotificationClient)))
			{
				*object = static_cast<IMMNotificationClient*>(this);
				AddRef();
				return S_OK;
			}
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() noexcept override
		{
			return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		ULONG STDMETHODCALLTYPE Release() noexcept override
		{
			const ULONG remaining =
				reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (remaining == 0)
				delete this;
			return remaining;
		}

		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
			LPCWSTR,
			DWORD) noexcept override
		{
			PublishChange();
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) noexcept override
		{
			PublishChange();
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) noexcept override
		{
			PublishChange();
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
			const EDataFlow flow,
			ERole,
			LPCWSTR) noexcept override
		{
			if (flow == eRender)
				PublishChange();
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
			LPCWSTR,
			const PROPERTYKEY) noexcept override
		{
			PublishChange();
			return S_OK;
		}
	};

	class AudioOutputDeviceNotificationService final
	{
		MusicPlayerLibrary::UniqueHandle stop_event_;
		std::mutex lifecycle_mutex_;
		std::condition_variable startup_cv_;
		std::jthread worker_;
		bool startup_complete_ = false;
		HRESULT startup_result_ = E_PENDING;
		bool shutdown_ = false;

		std::mutex subscribers_mutex_;
		std::unordered_map<std::uint64_t, IAudioOutputDeviceChangeSink*>
			subscribers_;
		std::uint64_t next_subscription_id_ = 0;
		std::atomic<std::uint64_t> revision_{0};

		void PublishStartupResult(const HRESULT result) noexcept
		{
			{
				std::lock_guard lock(lifecycle_mutex_);
				startup_result_ = result;
				startup_complete_ = true;
			}
			startup_cv_.notify_all();
		}

		void WorkerMain() noexcept
		{
			MusicPlayerLibrary::ComApartment com;
			if (FAILED(com.Result()))
			{
				PublishStartupResult(com.Result());
				return;
			}

			Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
			HRESULT result = CoCreateInstance(
				__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				IID_PPV_ARGS(enumerator.GetAddressOf()));
			if (FAILED(result) || !enumerator)
			{
				PublishStartupResult(FAILED(result) ? result : E_NOINTERFACE);
				return;
			}

			auto* notification_client =
				new (std::nothrow) EndpointNotificationClient(this);
			if (!notification_client)
			{
				PublishStartupResult(E_OUTOFMEMORY);
				return;
			}

			result = enumerator->RegisterEndpointNotificationCallback(
				notification_client);
			PublishStartupResult(result);
			if (FAILED(result))
			{
				notification_client->Detach();
				notification_client->Release();
				return;
			}

			(void)WaitForSingleObject(stop_event_.Get(), INFINITE);
			notification_client->Detach();
			(void)enumerator->UnregisterEndpointNotificationCallback(
				notification_client);
			notification_client->Release();
		}

		bool EnsureStarted() noexcept
		{
			std::unique_lock lock(lifecycle_mutex_);
			if (shutdown_)
				return false;
			if (worker_.joinable())
			{
				startup_cv_.wait(lock, [this] { return startup_complete_; });
				return SUCCEEDED(startup_result_);
			}

			stop_event_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
			if (!stop_event_)
			{
				startup_complete_ = true;
				startup_result_ = HRESULT_FROM_WIN32(GetLastError());
				return false;
			}
			startup_complete_ = false;
			startup_result_ = E_PENDING;
			try
			{
				worker_ = std::jthread([this] { WorkerMain(); });
			}
			catch (...)
			{
				startup_complete_ = true;
				startup_result_ = E_OUTOFMEMORY;
				return false;
			}
			startup_cv_.wait(lock, [this] { return startup_complete_; });
			return SUCCEEDED(startup_result_);
		}

	public:
		[[nodiscard]] std::uint64_t Subscribe(
			IAudioOutputDeviceChangeSink* sink)
		{
			if (!sink || !EnsureStarted())
				return 0;
			std::lock_guard lifecycle_lock(lifecycle_mutex_);
			if (shutdown_ || FAILED(startup_result_))
				return 0;
			std::lock_guard subscribers_lock(subscribers_mutex_);
			const auto subscription_id = ++next_subscription_id_;
			subscribers_.emplace(subscription_id, sink);
			return subscription_id;
		}

		void Unsubscribe(const std::uint64_t subscription_id) noexcept
		{
			if (subscription_id == 0)
				return;
			std::lock_guard lock(subscribers_mutex_);
			subscribers_.erase(subscription_id);
		}

		void PublishChange() noexcept
		{
			revision_.fetch_add(1, std::memory_order_acq_rel);
			// Explicit endpoint cache entries include the format probed when they
			// were created. Drop those strong references before asking managed code
			// to rebuild, while allowing existing sinks to finish their teardown.
			MusicPlayerLibrary::WasapiExclusiveOutputDevice::InvalidateShared();

			std::lock_guard lock(subscribers_mutex_);
			for (const auto& [subscription_id, sink] : subscribers_)
			{
				(void)subscription_id;
				if (sink)
					sink->OnAudioOutputDeviceChanged();
			}
		}

		[[nodiscard]] std::uint64_t GetRevision() noexcept
		{
			// Baseline reads must arm IMMNotificationClient before the first sink
			// is constructed; otherwise a device change during that construction
			// window would leave the revision unchanged and go undetected.
			(void)EnsureStarted();
			return revision_.load(std::memory_order_acquire);
		}

		void Shutdown() noexcept
		{
			std::jthread worker;
			{
				std::lock_guard lock(lifecycle_mutex_);
				if (shutdown_)
					return;
				shutdown_ = true;
				if (stop_event_)
					SetEvent(stop_event_.Get());
				worker = std::move(worker_);
			}
			if (worker.joinable())
			{
				worker.request_stop();
				worker.join();
			}
			std::lock_guard subscribers_lock(subscribers_mutex_);
			subscribers_.clear();
		}
	};

	void EndpointNotificationClient::PublishChange() noexcept
	{
		if (auto* owner = owner_.load(std::memory_order_acquire))
			owner->PublishChange();
	}

	AudioOutputDeviceNotificationService& GetNotificationService() noexcept
	{
		// The native runtime explicitly stops the worker. Keeping the small service
		// object alive avoids static-destruction ordering hazards with CLR finalizers.
		static auto* service = new AudioOutputDeviceNotificationService();
		return *service;
	}
}

MusicPlayerLibrary::AudioOutputDeviceChangeSubscription::
	AudioOutputDeviceChangeSubscription(
		const std::uint64_t subscription_id) noexcept :
	subscription_id_(subscription_id)
{
}

MusicPlayerLibrary::AudioOutputDeviceChangeSubscription::
	~AudioOutputDeviceChangeSubscription()
{
	GetNotificationService().Unsubscribe(subscription_id_);
}

std::unique_ptr<MusicPlayerLibrary::AudioOutputDeviceChangeSubscription>
MusicPlayerLibrary::SubscribeAudioOutputDeviceChanges(
	IAudioOutputDeviceChangeSink* sink)
{
	auto& service = GetNotificationService();
	const auto subscription_id = service.Subscribe(sink);
	if (subscription_id == 0)
		return nullptr;
	try
	{
		return std::unique_ptr<AudioOutputDeviceChangeSubscription>(
			new AudioOutputDeviceChangeSubscription(subscription_id));
	}
	catch (...)
	{
		service.Unsubscribe(subscription_id);
		throw;
	}
}

std::uint64_t MusicPlayerLibrary::
GetAudioOutputDeviceChangeRevision() noexcept
{
	return GetNotificationService().GetRevision();
}

void MusicPlayerLibrary::ShutdownAudioOutputDeviceNotifications() noexcept
{
	GetNotificationService().Shutdown();
}

#endif // defined(_WIN32)
