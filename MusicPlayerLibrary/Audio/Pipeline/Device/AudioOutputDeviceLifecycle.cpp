// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/Pipeline/Device/AudioOutputDeviceLifecycle.h"

#include "Audio/Pipeline/Device/Common/FAudioOutputDevice.h"
#if defined(_WIN32)
#include "Audio/Pipeline/Device/Windows/WasapiExclusiveOutputDevice.h"
#endif

void MusicPlayerLibrary::ShutdownAudioOutputDevices() noexcept
{
	FAudioOutputDevice::ShutdownShared();
#if defined(_WIN32)
	WasapiExclusiveOutputDevice::ShutdownShared();
#endif
}
