// SPDX-License-Identifier: MIT

#pragma once

#if defined(__cplusplus_cli)

#include <cstddef>
#include <type_traits>
#include <utility>
#include <Unknwn.h>

// WRL cannot be compiled in /clr mode. Keep the compatible implementation in
// the Windows platform layer so native Windows code has one include path.
namespace Microsoft::WRL
{
	template <typename T>
	class ComPtr final
	{
		static_assert(std::is_base_of_v<IUnknown, T>,
			"T needs to be IUnknown based");
		T* value_ = nullptr;

	public:
		ComPtr() = default;
		ComPtr(std::nullptr_t) noexcept {}
		explicit ComPtr(T* value) : value_(value)
		{
			if (value_)
				value_->AddRef();
		}

		ComPtr(const ComPtr& other) : ComPtr(other.value_) {}
		ComPtr(ComPtr&& other) noexcept : value_(other.Detach()) {}
		~ComPtr() { Reset(); }

		ComPtr& operator=(const ComPtr& other)
		{
			if (this != &other)
			{
				ComPtr copy(other);
				Swap(copy);
			}
			return *this;
		}

		ComPtr& operator=(ComPtr&& other) noexcept
		{
			if (this != &other)
				Attach(other.Detach());
			return *this;
		}

		[[nodiscard]] T* Get() const noexcept { return value_; }
		[[nodiscard]] T* GetInterface() const noexcept { return value_; }
		[[nodiscard]] T* operator->() const noexcept { return value_; }
		[[nodiscard]] T& operator*() const noexcept { return *value_; }
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return value_ != nullptr;
		}
		[[nodiscard]] bool operator!() const noexcept { return value_ == nullptr; }

		[[nodiscard]] T** GetAddressOf() noexcept { return &value_; }
		[[nodiscard]] T** ReleaseAndGetAddressOf() noexcept
		{
			Reset();
			return &value_;
		}

		void Attach(T* value) noexcept
		{
			Reset();
			value_ = value;
		}

		[[nodiscard]] T* Detach() noexcept
		{
			T* result = value_;
			value_ = nullptr;
			return result;
		}

		void Reset() noexcept
		{
			if (value_)
			{
				value_->Release();
				value_ = nullptr;
			}
		}

		void Swap(ComPtr& other) noexcept
		{
			std::swap(value_, other.value_);
		}

		template <typename Interface>
		HRESULT QueryInterface(Interface** interface_pointer) const
		{
			return value_->QueryInterface(IID_PPV_ARGS(interface_pointer));
		}
	};
}

#else

#include <wrl/client.h>

#endif
