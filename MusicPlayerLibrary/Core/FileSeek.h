// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <limits>

#include "Core/FileAbstractionLayer.h"

namespace MusicPlayerLibrary
{
    [[nodiscard]] inline bool TryResolveFileSeekPosition(
        const std::uint64_t current_position,
        const std::uint64_t file_length,
        const std::int64_t offset,
        const FileSeekOrigin origin,
        std::uint64_t& resolved_position) noexcept
    {
        std::uint64_t base_position;
        switch (origin)
        {
        case FileSeekOrigin::Begin:
            base_position = 0;
            break;
        case FileSeekOrigin::Current:
            base_position = current_position;
            break;
        case FileSeekOrigin::End:
            base_position = file_length;
            break;
        default:
            base_position = 0;
            break;
        }

        if (offset < 0)
        {
            const auto distance = static_cast<std::uint64_t>(-(offset + 1)) + 1;
            if (distance > base_position)
                return false;
            resolved_position = base_position - distance;
            return true;
        }

        const auto distance = static_cast<std::uint64_t>(offset);
        if (base_position > (std::numeric_limits<std::uint64_t>::max)() - distance)
            return false;
        resolved_position = base_position + distance;
        return true;
    }

	[[nodiscard]] inline bool TrySeekFileAbsolute(
		IFile& file,
		const std::uint64_t position)
	{
		if (position > static_cast<std::uint64_t>(
			(std::numeric_limits<std::int64_t>::max)()))
		{
			return false;
		}
		return file.Seek(
			static_cast<std::int64_t>(position), FileSeekOrigin::Begin) == position;
	}
}
