// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Core/AudioThreadScheduleHelper.h"

namespace MusicPlayerLibrary
{
    IAudioThreadScheduleHelper::~IAudioThreadScheduleHelper() { }

	std::unique_ptr<IAudioThreadScheduleHelper>
	CreateDefaultAudioThreadScheduleHelper(
		const wchar_t* task_name,
		const MPL_AUDIO_PRIORITY priority,
		const char* worker_name)
	{
		auto* factory = GetDefaultAudioThreadSchedulerFactory();
		return factory
			? factory->CreateAudioThreadScheduleHelper(
				task_name, priority, worker_name)
			: nullptr;
	}
}
