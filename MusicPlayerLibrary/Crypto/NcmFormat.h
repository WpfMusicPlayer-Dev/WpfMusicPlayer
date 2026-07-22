// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace MusicPlayerLibrary
{
	inline constexpr std::array<char, 8> NcmMagicHeader{
		'C', 'T', 'E', 'N', 'F', 'D', 'A', 'M'
	};
	inline constexpr std::uint64_t NcmKeySectionOffset = 10;
	inline constexpr std::uint64_t NcmAudioDataLengthFieldOffset = 5;
	inline constexpr std::uint64_t NcmAudioHeaderSize = 13;
	inline constexpr std::uint64_t NcmAudioHeaderPrefixSize =
		NcmAudioDataLengthFieldOffset + sizeof(std::uint32_t);

	[[nodiscard]] inline constexpr bool IsNcmMagicHeader(
		const std::span<const char> bytes) noexcept
	{
		if (bytes.size() < NcmMagicHeader.size())
			return false;
		for (std::size_t index = 0; index < NcmMagicHeader.size(); ++index)
			if (bytes[index] != NcmMagicHeader[index])
				return false;
		return true;
	}
}
