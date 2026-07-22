// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>

namespace MusicPlayerLibrary
{
	[[nodiscard]] inline std::int64_t SteadyClockNanoseconds(
		const std::chrono::steady_clock::time_point time_point) noexcept
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			time_point.time_since_epoch()).count();
	}

	[[nodiscard]] inline std::int64_t SteadyClockNowNanoseconds() noexcept
	{
		return SteadyClockNanoseconds(std::chrono::steady_clock::now());
	}
}
