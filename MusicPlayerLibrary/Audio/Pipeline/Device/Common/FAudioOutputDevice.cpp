// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

#include "Audio/Pipeline/Device/Common/FAudioOutputDevice.h"
#include "Audio/Pipeline/Device/Common/SharedAudioDeviceCache.h"
#include "Core/Utf16Conversion.h"

namespace
{
	MusicPlayerLibrary::SharedAudioDeviceCache<
		MusicPlayerLibrary::FAudioOutputDevice> SharedDeviceCache(
			"FAudio output device has already shut down");

	MusicPlayerLibrary::AudioOutputDeviceInfo MakeDeviceInfo(
		const FAudioDeviceDetails& details,
		const std::uint32_t index)
	{
		MusicPlayerLibrary::AudioOutputDeviceInfo result{
			.id = MusicPlayerLibrary::ConvertUtf16CodeUnitsToUtf8(
				details.DeviceID, std::size(details.DeviceID), true),
			.display_name = MusicPlayerLibrary::ConvertUtf16CodeUnitsToUtf8(
				details.DisplayName, std::size(details.DisplayName), true),
			.is_default = index == 0 || details.Role != FAudioNotDefaultDevice
		};
		if (result.display_name.empty())
			result.display_name = result.id.empty() ? "Default audio device" : result.id;
		return result;
	}

	struct FAudioDeviceEntry
	{
		std::uint32_t index = 0;
		FAudioDeviceDetails details{};
		MusicPlayerLibrary::AudioOutputDeviceInfo info{};
	};

	std::vector<FAudioDeviceEntry> EnumerateFAudioDeviceEntries(FAudio* engine)
	{
		std::uint32_t device_count = 0;
		if (!engine ||
			FAudio_GetDeviceCount(engine, &device_count) != FAUDIO_OK)
		{
			throw std::runtime_error("FAudio_GetDeviceCount failed");
		}

		std::vector<FAudioDeviceEntry> result;
		result.reserve(device_count);
		for (std::uint32_t index = 0; index < device_count; ++index)
		{
			FAudioDeviceDetails details{};
			if (FAudio_GetDeviceDetails(engine, index, &details) == FAUDIO_OK)
			{
				result.push_back({
					.index = index,
					.details = details,
					.info = MakeDeviceInfo(details, index)
				});
			}
		}
		return result;
	}

	struct SelectedFAudioDevice
	{
		FAudioDeviceEntry device{};
		bool requested_id_matched = false;
	};

	SelectedFAudioDevice SelectDevice(
		FAudio* engine,
		const std::string& requested_device_id)
	{
		auto devices = EnumerateFAudioDeviceEntries(engine);
		if (devices.empty())
			throw std::runtime_error("FAudio did not report an output device");

		std::optional<FAudioDeviceEntry> default_device;
		for (auto& device : devices)
		{
			if (device.index == 0)
				default_device = device;
			if ((requested_device_id.empty() && device.index == 0) ||
				(!requested_device_id.empty() &&
					device.info.id == requested_device_id))
			{
				return {
					.device = std::move(device),
					.requested_id_matched = true
				};
			}
		}

		if (!requested_device_id.empty() && default_device)
			return { .device = std::move(*default_device) };
		throw std::runtime_error("FAudio could not open its default output device");
	}

}

MusicPlayerLibrary::FAudioOutputDevice::FAudioOutputDevice(
	const AudioOutputFormat& requested)
{
	engine_ = CreateFAudioEngine("FAudioCreate failed");

	const auto selected = SelectDevice(engine_.get(), requested.requested_device_id);
	device_index_ = selected.device.index;
	device_info_ = selected.device.info;
	system_format_ = selected.device.details.OutputFormat;
	if (system_format_.Format.nChannels == 0 ||
		system_format_.Format.nSamplesPerSec == 0)
	{
		system_format_ = MakeFallbackAudioWaveFormat();
	}

	AudioOutputFormat normalized_request = requested;
	if (!requested.requested_device_id.empty() &&
		!selected.requested_id_matched)
	{
		NATIVE_TRACE(
			"warn: FAudio could not map requested output device id '%s'; "
			"clearing the selection and using default device '%s'\n",
			requested.requested_device_id.c_str(),
			device_info_.id.c_str());
		normalized_request.requested_device_id.clear();
	}
	output_format_ = ResolveAudioOutputFormat(normalized_request, system_format_);

	FAudioMasteringVoice* raw_mastering_voice = nullptr;
	const auto create_mastering_voice_result = FAudio_CreateMasteringVoice(
		engine_.get(),
		&raw_mastering_voice,
		output_format_.channel_count,
		output_format_.sample_rate,
		0,
		device_index_,
		nullptr);
	UniqueFAudioVoice mastering_voice(raw_mastering_voice);
	if (create_mastering_voice_result != FAUDIO_OK || !mastering_voice)
	{
		throw std::runtime_error("FAudio_CreateMasteringVoice failed");
	}
	mastering_voice_ = std::move(mastering_voice);
}

std::shared_ptr<MusicPlayerLibrary::FAudioOutputDevice>
MusicPlayerLibrary::FAudioOutputDevice::Acquire(const AudioOutputFormat& requested)
{
	return SharedDeviceCache.Acquire(
		requested.requested_device_id,
		[&requested]
		{
			return std::shared_ptr<FAudioOutputDevice>(
				new FAudioOutputDevice(requested));
		},
		[](const FAudioOutputDevice& device)
		{
			// A stale or foreign ID is normalized to the dynamic default and
			// deliberately remains uncached.
			return !device.GetOutputFormat().requested_device_id.empty();
		});
}

std::vector<MusicPlayerLibrary::AudioOutputDeviceInfo>
MusicPlayerLibrary::FAudioOutputDevice::EnumerateOutputDevices()
{
	auto engine = CreateFAudioEngine(
		"FAudioCreate failed while enumerating output devices");

	auto devices = EnumerateFAudioDeviceEntries(engine.get());
	std::vector<AudioOutputDeviceInfo> result;
	result.reserve(devices.size());
	for (auto& device : devices)
		result.push_back(std::move(device.info));
	return result;
}

void MusicPlayerLibrary::FAudioOutputDevice::ShutdownShared() noexcept
{
	SharedDeviceCache.Shutdown();
}

MusicPlayerLibrary::AudioOutputFormat
MusicPlayerLibrary::FAudioOutputDevice::ResolveSinkFormat(
	const AudioOutputFormat& requested) const
{
	AudioOutputFormat normalized_request = requested;
	if (!requested.requested_device_id.empty() &&
		requested.requested_device_id != device_info_.id)
	{
		normalized_request.requested_device_id.clear();
	}
	return ResolveAudioOutputFormat(normalized_request, system_format_);
}

std::uint32_t
MusicPlayerLibrary::FAudioOutputDevice::GetCurrentLatencyInSamples() const noexcept
{
	if (!engine_)
		return 0;
	FAudioPerformanceData performance{};
	FAudio_GetPerformanceData(engine_.get(), &performance);
	return performance.CurrentLatencyInSamples;
}

void MusicPlayerLibrary::FAudioOutputDevice::SetMasterVolume(float volume) noexcept
{
	volume = std::clamp(volume, 0.0f, 1.0f);
	if (mastering_voice_)
		(void)FAudioVoice_SetVolume(mastering_voice_.get(), volume, FAUDIO_COMMIT_NOW);
}

MusicPlayerLibrary::FAudioOutputDevice::~FAudioOutputDevice() = default;
