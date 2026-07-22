// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>

namespace MusicPlayerLibrary 
{
	class LocaleConverter
	{
	public:
		static std::string GetUtf8StringFromBytes(
			const char* input,
			std::size_t size);
		static bool IsUtf8CompatibleBytes(
			const char* input,
			std::size_t size);
		static std::wstring GetUtf16StringFromUtf8String(const std::string& input);
		static std::string GetUtf8StringFromUtf16String(const std::wstring& input);
	};
}
