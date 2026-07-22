// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <limits>

namespace MusicPlayerLibrary
{
	[[nodiscard]] inline constexpr std::uint32_t SaturateToUint32(
		const std::uint64_t value) noexcept
	{
		constexpr auto Maximum = (std::numeric_limits<std::uint32_t>::max)();
		return value > Maximum ? Maximum : static_cast<std::uint32_t>(value);
	}
}
