// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/FFmpeg/FFmpegError.h"

#include <array>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/error.h>
#if defined(__cplusplus)
}
#endif

std::string MusicPlayerLibrary::GetFFmpegErrorMessage(const int error_code)
{
	std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
	if (av_strerror(error_code, message.data(), message.size()) < 0)
		return "unknown FFmpeg error " + std::to_string(error_code);
	return message.data();
}
