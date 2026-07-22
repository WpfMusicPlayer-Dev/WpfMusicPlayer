// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "Core/FileAbstractionLayer.h"
#include "Core/NumericConversion.h"

namespace MusicPlayerLibrary
{
    [[nodiscard]] inline bool ReadExact(
        IFile& file,
        void* const destination,
        const std::size_t size) noexcept
    {
        if (size == 0)
            return true;
        if (destination == nullptr)
            return false;

        auto* const bytes = static_cast<std::byte*>(destination);
        std::size_t total_read = 0;
        try
        {
            while (total_read < size)
            {
                const auto request_size = SaturateToUint32(size - total_read);
                const auto bytes_read = file.Read(bytes + total_read, request_size);
                if (bytes_read == 0 || bytes_read > request_size)
                    return false;
                total_read += bytes_read;
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    [[nodiscard]] inline bool ReadExact(IFile& file, const std::span<std::byte> destination) noexcept
    {
        return ReadExact(file, destination.data(), destination.size_bytes());
    }

	[[nodiscard]] inline std::vector<std::uint8_t> ReadToEnd(IFile& file)
	{
		std::vector<std::uint8_t> result;
		std::array<std::uint8_t, 4'096> buffer{};
		for (;;)
		{
			const std::uint32_t bytes_read = file.Read(
				buffer.data(), static_cast<std::uint32_t>(buffer.size()));
			if (bytes_read == 0)
				return result;
			if (bytes_read > buffer.size())
				throw std::runtime_error("IFile::Read returned an invalid byte count");
			result.insert(
				result.end(), buffer.begin(), buffer.begin() + bytes_read);
		}
	}
}
