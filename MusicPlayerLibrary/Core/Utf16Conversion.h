// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace MusicPlayerLibrary
{
	template <typename Utf16CodeUnit>
	[[nodiscard]] std::string ConvertUtf16CodeUnitsToUtf8(
		const Utf16CodeUnit* const input,
		const std::size_t code_unit_count,
		const bool stop_at_null = false)
	{
		static_assert(std::is_integral_v<Utf16CodeUnit>);
		static_assert(sizeof(Utf16CodeUnit) == sizeof(std::uint16_t));
		if (input == nullptr || code_unit_count == 0)
			return {};

		std::string result;
		result.reserve(code_unit_count);
		for (std::size_t index = 0; index < code_unit_count; ++index)
		{
			const auto first = static_cast<std::uint16_t>(input[index]);
			if (stop_at_null && first == 0)
				break;

			std::uint32_t code_point = first;
			if (first >= 0xd800 && first <= 0xdbff)
			{
				if (index + 1 < code_unit_count)
				{
					const auto second = static_cast<std::uint16_t>(input[index + 1]);
					if (second >= 0xdc00 && second <= 0xdfff)
					{
						code_point = 0x10000u +
							((static_cast<std::uint32_t>(first) - 0xd800u) << 10) +
							(static_cast<std::uint32_t>(second) - 0xdc00u);
						++index;
					}
					else
					{
						code_point = 0xfffd;
					}
				}
				else
				{
					code_point = 0xfffd;
				}
			}
			else if (first >= 0xdc00 && first <= 0xdfff)
			{
				code_point = 0xfffd;
			}

			if (code_point <= 0x7f)
			{
				result.push_back(static_cast<char>(code_point));
			}
			else if (code_point <= 0x7ff)
			{
				result.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
				result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
			}
			else if (code_point <= 0xffff)
			{
				result.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
				result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
				result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
			}
			else
			{
				result.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
				result.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
				result.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
				result.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
			}
		}
		return result;
	}
}
