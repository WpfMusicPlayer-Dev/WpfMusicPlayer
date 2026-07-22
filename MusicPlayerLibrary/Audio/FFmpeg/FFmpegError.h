// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace MusicPlayerLibrary
{
	[[nodiscard]] std::string GetFFmpegErrorMessage(int error_code);
}
