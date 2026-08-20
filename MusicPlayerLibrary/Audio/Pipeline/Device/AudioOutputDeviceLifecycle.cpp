// SPDX-License-Identifier: MIT

#include "pch.h"

#include "Audio/Pipeline/Device/AudioOutputDeviceLifecycle.h"

#include "Audio/Pipeline/Device/Common/FAudioOutputDevice.h"
#if defined(_WIN32)
#include "Audio/Pipeline/Device/Windows/AudioOutputDeviceNotification.h"
#include "Audio/Pipeline/Device/Windows/WasapiExclusiveOutputDevice.h"
#endif

void MusicPlayerLibrary::InvalidateAudioOutputDevices() noexcept
{
	FAudioOutputDevice::InvalidateShared();
#if defined(_WIN32)
	WasapiExclusiveOutputDevice::InvalidateShared();
#endif
}

void MusicPlayerLibrary::ShutdownAudioOutputDevices() noexcept
{
#if defined(_WIN32)
	ShutdownAudioOutputDeviceNotifications();
#endif
	FAudioOutputDevice::ShutdownShared();
#if defined(_WIN32)
	WasapiExclusiveOutputDevice::ShutdownShared();
#endif
}
