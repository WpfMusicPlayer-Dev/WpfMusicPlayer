// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string_view>

namespace MusicPlayerLibrary
{
	template <typename Character>
	[[nodiscard]] constexpr Character ToLowerAscii(
		const Character value) noexcept
	{
		return value >= static_cast<Character>('A') &&
			value <= static_cast<Character>('Z')
			? static_cast<Character>(value +
				(static_cast<Character>('a') - static_cast<Character>('A')))
			: value;
	}

	template <typename Character>
	[[nodiscard]] constexpr bool EqualsAsciiIgnoreCase(
		const std::basic_string_view<Character> left,
		const std::basic_string_view<Character> right) noexcept
	{
		if (left.size() != right.size())
			return false;

		for (std::size_t index = 0; index < left.size(); ++index)
		{
			if (ToLowerAscii(left[index]) != ToLowerAscii(right[index]))
				return false;
		}
		return true;
	}
}
