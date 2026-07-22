// SPDX-License-Identifier: MIT

#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <objbase.h>
#include <Propvarutil.h>

#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

namespace MusicPlayerLibrary
{
	struct CoTaskMemDeleter final
	{
		template <typename T>
		void operator()(T* value) const noexcept
		{
			CoTaskMemFree(value);
		}
	};

	template <typename T>
	using UniqueCoTaskMem = std::unique_ptr<T, CoTaskMemDeleter>;

	class ScopedPropVariant final
	{
		PROPVARIANT value_{};

	public:
		ScopedPropVariant() noexcept { PropVariantInit(&value_); }
		~ScopedPropVariant() { PropVariantClear(&value_); }

		ScopedPropVariant(const ScopedPropVariant&) = delete;
		ScopedPropVariant& operator=(const ScopedPropVariant&) = delete;

		[[nodiscard]] PROPVARIANT* GetAddressOf() noexcept { return &value_; }
		[[nodiscard]] const PROPVARIANT& Get() const noexcept { return value_; }
	};

	class ComApartment final
	{
		HRESULT result_ = E_FAIL;

	public:
		explicit ComApartment(
			const DWORD concurrency_model = COINIT_MULTITHREADED) noexcept :
			result_(CoInitializeEx(nullptr, concurrency_model))
		{
		}

		~ComApartment()
		{
			if (SUCCEEDED(result_))
				CoUninitialize();
		}

		ComApartment(const ComApartment&) = delete;
		ComApartment& operator=(const ComApartment&) = delete;

		[[nodiscard]] HRESULT Result() const noexcept { return result_; }
	};

	[[nodiscard]] inline std::uint32_t HResultCode(
		const HRESULT result) noexcept
	{
		return static_cast<std::uint32_t>(result);
	}

	[[nodiscard]] inline std::string FormatHResult(
		const char* operation,
		const HRESULT result)
	{
		return std::format("{} failed (HRESULT 0x{:08X})", operation,
			HResultCode(result));
	}

	[[noreturn]] inline void ThrowHResult(
		const char* operation,
		const HRESULT result)
	{
		throw std::runtime_error(FormatHResult(operation, result));
	}

	class UniqueHandle final
	{
		HANDLE value_ = nullptr;

		[[nodiscard]] static bool IsValid(const HANDLE value) noexcept
		{
			return value != nullptr && value != INVALID_HANDLE_VALUE;
		}

	public:
		UniqueHandle() = default;
		explicit UniqueHandle(const HANDLE value) noexcept : value_(value) {}
		~UniqueHandle() { Reset(); }

		UniqueHandle(const UniqueHandle&) = delete;
		UniqueHandle& operator=(const UniqueHandle&) = delete;

		UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}
		UniqueHandle& operator=(UniqueHandle&& other) noexcept
		{
			if (this != &other)
				Reset(other.Release());
			return *this;
		}

		[[nodiscard]] HANDLE Get() const noexcept { return value_; }
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return IsValid(value_);
		}

		void Reset(const HANDLE value = nullptr) noexcept
		{
			if (IsValid(value_))
				CloseHandle(value_);
			value_ = value;
		}

		[[nodiscard]] HANDLE Release() noexcept
		{
			const HANDLE result = value_;
			value_ = nullptr;
			return result;
		}
	};
}

#endif
