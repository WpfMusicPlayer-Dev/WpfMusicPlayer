// SPDX-License-Identifier: MIT

#include "pch.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <mmreg.h>
#include <Propvarutil.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <vector>

#include "Audio/Pipeline/Device/Common/SharedAudioDeviceCache.h"
#include "Audio/Pipeline/Device/Windows/WasapiExclusiveOutputDevice.h"
#include "Audio/Pipeline/Windows/WasapiAudioHelpers.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WindowsResource.h"

#include "Core/LocaleConverter.h"

namespace
{
	using MusicPlayerLibrary::AudioBackend;
	using MusicPlayerLibrary::AudioBitDepth;
	using MusicPlayerLibrary::AudioChannelMode;
	using MusicPlayerLibrary::AudioOutputDeviceInfo;
	using MusicPlayerLibrary::AudioOutputFormat;
	using MusicPlayerLibrary::WasapiExclusiveOutputDevice;

	MusicPlayerLibrary::SharedAudioDeviceCache<WasapiExclusiveOutputDevice>
		SharedDeviceCache("WASAPI exclusive output device has already shut down");

	Microsoft::WRL::ComPtr<IMMDeviceEnumerator> CreateDeviceEnumerator()
	{
		Microsoft::WRL::ComPtr<IMMDeviceEnumerator> result;
		const HRESULT status = CoCreateInstance(
			__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			IID_PPV_ARGS(result.GetAddressOf()));
		if (FAILED(status))
			MusicPlayerLibrary::ThrowHResult(
				"CoCreateInstance(MMDeviceEnumerator)", status);
		return result;
	}

	std::wstring ReadEndpointId(IMMDevice* endpoint)
	{
		LPWSTR raw_id = nullptr;
		const HRESULT status = endpoint ? endpoint->GetId(&raw_id) : E_POINTER;
		MusicPlayerLibrary::UniqueCoTaskMem<wchar_t> endpoint_id(raw_id);
		if (FAILED(status) || !endpoint_id)
			MusicPlayerLibrary::ThrowHResult("IMMDevice::GetId", status);
		return std::wstring(endpoint_id.get());
	}

	std::wstring ReadFriendlyName(IMMDevice* endpoint)
	{
		Microsoft::WRL::ComPtr<IPropertyStore> properties;
		if (!endpoint || FAILED(endpoint->OpenPropertyStore(
			STGM_READ, properties.GetAddressOf())))
		{
			return {};
		}

		MusicPlayerLibrary::ScopedPropVariant value;
		std::wstring result;
		if (SUCCEEDED(properties->GetValue(
			PKEY_Device_FriendlyName, value.GetAddressOf())) &&
			value.Get().vt == VT_LPWSTR && value.Get().pwszVal)
		{
			result = value.Get().pwszVal;
		}
		return result;
	}

	std::wstring TryReadDefaultEndpointId(IMMDeviceEnumerator* enumerator)
	{
		Microsoft::WRL::ComPtr<IMMDevice> endpoint;
		if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(
			eRender, eMultimedia, endpoint.GetAddressOf())))
		{
			return {};
		}
		try
		{
			return ReadEndpointId(endpoint.Get());
		}
		catch (...)
		{
			return {};
		}
	}

	AudioOutputDeviceInfo MakeDeviceInfo(
		IMMDevice* endpoint,
		const std::wstring& default_endpoint_id)
	{
		const std::wstring endpoint_id = ReadEndpointId(endpoint);
		std::wstring display_name = ReadFriendlyName(endpoint);
		if (display_name.empty())
			display_name = endpoint_id;
		return {
			.id = MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(endpoint_id),
			.display_name = MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(display_name),
			.is_default = !default_endpoint_id.empty() &&
				endpoint_id == default_endpoint_id
		};
	}

	Microsoft::WRL::ComPtr<IMMDevice> SelectEndpoint(
		IMMDeviceEnumerator* enumerator,
		const std::string& requested_device_id)
	{
		Microsoft::WRL::ComPtr<IMMDevice> endpoint;
		const HRESULT status = requested_device_id.empty()
			? enumerator->GetDefaultAudioEndpoint(
				eRender, eMultimedia, endpoint.GetAddressOf())
			: enumerator->GetDevice(
				 MusicPlayerLibrary::LocaleConverter::GetUtf16StringFromUtf8String(requested_device_id).c_str(), endpoint.GetAddressOf());
		if (FAILED(status) || !endpoint)
			MusicPlayerLibrary::ThrowHResult(
				"Unable to select the WASAPI output endpoint", status);
		return endpoint;
	}

	Microsoft::WRL::ComPtr<IAudioClient> ActivateEndpointAudioClient(IMMDevice* endpoint)
	{
		Microsoft::WRL::ComPtr<IAudioClient> client;
		const HRESULT status = endpoint
			? endpoint->Activate(
				__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
				reinterpret_cast<void**>(client.GetAddressOf()))
			: E_POINTER;
		if (FAILED(status) || !client)
			MusicPlayerLibrary::ThrowHResult(
				"IMMDevice::Activate(IAudioClient)", status);
		return client;
	}

	AudioOutputFormat ProbeExclusiveFormat(
		IAudioClient* client,
		const std::string& endpoint_id)
	{
		if (!client)
			MusicPlayerLibrary::ThrowHResult(
				"IAudioClient::GetMixFormat", E_POINTER);

		WAVEFORMATEX* raw_mix_format = nullptr;
		const HRESULT mix_status = client->GetMixFormat(&raw_mix_format);
		MusicPlayerLibrary::UniqueCoTaskMem<WAVEFORMATEX> mix_format(
			raw_mix_format);
		if (FAILED(mix_status) || !mix_format)
		{
			MusicPlayerLibrary::ThrowHResult(
				"IAudioClient::GetMixFormat",
				FAILED(mix_status) ? mix_status : E_POINTER);
		}

		FAudioWaveFormatExtensible system_format{};
		if (!MusicPlayerLibrary::TryToFAudioWaveFormatExtensible(
			mix_format.get(), system_format))
		{
			MusicPlayerLibrary::ThrowHResult(
				"Unsupported endpoint mix format", E_INVALIDARG);
		}

		AudioOutputFormat normalized_request{};
		normalized_request.requested_backend = AudioBackend::WasapiExclusive;
		normalized_request.requested_device_id = endpoint_id;
		const auto preferred = MusicPlayerLibrary::ResolveAudioOutputFormat(
			normalized_request, system_format);

		NATIVE_TRACE(
			"info: probing WASAPI exclusive formats from endpoint recommendation: "
			"%luHz/%uch/tag0x%04X/%ubit\n",
			static_cast<unsigned long>(system_format.Format.nSamplesPerSec),
			static_cast<unsigned int>(system_format.Format.nChannels),
			static_cast<unsigned int>(system_format.Format.wFormatTag),
			static_cast<unsigned int>(system_format.Format.wBitsPerSample));

		HRESULT last_status = AUDCLNT_E_UNSUPPORTED_FORMAT;
		std::vector<WAVEFORMATEXTENSIBLE> attempted_formats;
		const auto try_candidate = [&](const AudioOutputFormat& candidate)
			-> bool
		{
			const auto wave_format =
				MusicPlayerLibrary::ToWindowsWaveFormatExtensible(candidate);
			if (std::any_of(
				attempted_formats.begin(), attempted_formats.end(),
				[&wave_format](const WAVEFORMATEXTENSIBLE& attempted)
				{
					return MusicPlayerLibrary::AreWasapiWireFormatsEqual(
						attempted, wave_format);
				}))
			{
				return false;
			}
			attempted_formats.push_back(wave_format);

			last_status = client->IsFormatSupported(
				AUDCLNT_SHAREMODE_EXCLUSIVE,
				&wave_format.Format,
				nullptr);
			if (last_status == S_OK)
				return true;
			if (last_status == S_FALSE)
			{
				last_status = AUDCLNT_E_UNSUPPORTED_FORMAT;
				return false;
			}
			if (FAILED(last_status) &&
				last_status != AUDCLNT_E_UNSUPPORTED_FORMAT)
			{
				MusicPlayerLibrary::ThrowHResult(
					"IAudioClient::IsFormatSupported", last_status);
			}
			return false;
		};

		AudioOutputFormat selected{};
		const auto try_candidate_variants =
			[&](const AudioOutputFormat& candidate) -> bool
		{
			if (!MusicPlayerLibrary::IsSerializableAudioOutputFormat(candidate))
				return false;
			if (try_candidate(candidate))
			{
				selected = candidate;
				return true;
			}

			AudioOutputFormat legacy{};
			if (MusicPlayerLibrary::TryMakeWasapiLegacyVariant(
				candidate, legacy) && try_candidate(legacy))
			{
				selected = legacy;
				return true;
			}

			if (candidate.bit_depth == AudioBitDepth::Bit32 &&
				candidate.sample_format == AV_SAMPLE_FMT_FLT)
			{
				const auto pcm32 =
					MusicPlayerLibrary::MakeWasapiPcm32Variant(candidate);
				if (try_candidate(pcm32))
				{
					selected = pcm32;
					return true;
				}
				if (MusicPlayerLibrary::TryMakeWasapiLegacyVariant(
					pcm32, legacy) && try_candidate(legacy))
				{
					selected = legacy;
					return true;
				}
			}
			return false;
		};

		const auto return_selected = [&]() -> AudioOutputFormat
		{
			const auto selected_wave =
				MusicPlayerLibrary::ToWindowsWaveFormatExtensible(selected);
			const auto valid_bits = selected_wave.Format.wFormatTag ==
				WAVE_FORMAT_EXTENSIBLE
				? selected_wave.Samples.wValidBitsPerSample
				: selected_wave.Format.wBitsPerSample;
			NATIVE_TRACE(
				"info: selected WASAPI exclusive format after %zu probe(s): "
				"%luHz/%uch/tag0x%04X/%ubit/%uvalid/mask0x%08lX/subformat0x%08lX\n",
				attempted_formats.size(),
				static_cast<unsigned long>(selected_wave.Format.nSamplesPerSec),
				static_cast<unsigned int>(selected_wave.Format.nChannels),
				static_cast<unsigned int>(selected_wave.Format.wFormatTag),
				static_cast<unsigned int>(selected_wave.Format.wBitsPerSample),
				static_cast<unsigned int>(valid_bits),
				static_cast<unsigned long>(selected_wave.dwChannelMask),
				static_cast<unsigned long>(selected_wave.SubFormat.Data1));
			return selected;
		};

		if (try_candidate_variants(preferred))
			return return_selected();

		const auto axes =
			MusicPlayerLibrary::BuildWasapiExclusiveProbeAxes(preferred);
		for (const int sample_rate : axes.sample_rates)
		{
			for (const AudioChannelMode channel_mode : axes.channel_modes)
			{
				for (const AudioBitDepth bit_depth : axes.bit_depths)
				{
					AudioOutputFormat request{};
					request.requested_backend = AudioBackend::WasapiExclusive;
					request.requested_device_id = endpoint_id;
					request.requested_sample_rate = sample_rate;
					request.requested_channel_mode = channel_mode;
					request.requested_bit_depth = bit_depth;
					auto candidate = MusicPlayerLibrary::ResolveAudioOutputFormat(
						request, system_format);
					if (try_candidate_variants(candidate))
						return return_selected();
				}
			}
		}

		NATIVE_TRACE(
			"warn: no supported WASAPI exclusive output format after %zu probe(s), "
			"last HRESULT=0x%08lX\n",
			attempted_formats.size(),
			static_cast<unsigned long>(last_status));
		MusicPlayerLibrary::ThrowHResult(
			"No supported WASAPI exclusive output format was found", last_status);
	}
}

MusicPlayerLibrary::WasapiExclusiveOutputDevice::WasapiExclusiveOutputDevice(
	const AudioOutputFormat& requested)
{
	ComApartment com;
	if (FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE)
		ThrowHResult("CoInitializeEx for WASAPI", com.Result());
	const auto enumerator = CreateDeviceEnumerator();
	const auto endpoint = SelectEndpoint(enumerator.Get(), requested.requested_device_id);
	endpoint_id_ = ReadEndpointId(endpoint.Get());
	device_info_ = MakeDeviceInfo(
		endpoint.Get(), TryReadDefaultEndpointId(enumerator.Get()));

	const auto client = ActivateEndpointAudioClient(endpoint.Get());
	REFERENCE_TIME default_period = 0;
	REFERENCE_TIME minimum_period = 0;
	const HRESULT period_status = client->GetDevicePeriod(
		&default_period, &minimum_period);
	if (FAILED(period_status))
		ThrowHResult("IAudioClient::GetDevicePeriod", period_status);
	default_device_period_100ns_ = default_period;
	minimum_device_period_100ns_ = minimum_period;
	output_format_ = ProbeExclusiveFormat(client.Get(), device_info_.id);
}

std::shared_ptr<MusicPlayerLibrary::WasapiExclusiveOutputDevice>
MusicPlayerLibrary::WasapiExclusiveOutputDevice::Acquire(
	const AudioOutputFormat& requested)
{
	return SharedDeviceCache.Acquire(
		requested.requested_device_id,
		[&requested]
		{
			return std::shared_ptr<WasapiExclusiveOutputDevice>(
				new WasapiExclusiveOutputDevice(requested));
		},
		[](const WasapiExclusiveOutputDevice&) { return true; });
}

std::vector<MusicPlayerLibrary::AudioOutputDeviceInfo>
MusicPlayerLibrary::WasapiExclusiveOutputDevice::EnumerateOutputDevices()
{
	ComApartment com;
	if (FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE)
		ThrowHResult("CoInitializeEx for WASAPI enumeration", com.Result());
	const auto enumerator = CreateDeviceEnumerator();
	const std::wstring default_endpoint_id =
		TryReadDefaultEndpointId(enumerator.Get());
	Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
	const HRESULT enumeration_status = enumerator->EnumAudioEndpoints(
		eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf());
	if (FAILED(enumeration_status))
		ThrowHResult(
			"IMMDeviceEnumerator::EnumAudioEndpoints", enumeration_status);

	UINT count = 0;
	const HRESULT count_status = collection->GetCount(&count);
	if (FAILED(count_status))
		ThrowHResult("IMMDeviceCollection::GetCount", count_status);

	std::vector<AudioOutputDeviceInfo> result;
	result.reserve(count);
	for (UINT index = 0; index < count; ++index)
	{
		Microsoft::WRL::ComPtr<IMMDevice> endpoint;
		if (SUCCEEDED(collection->Item(index, endpoint.GetAddressOf())) && endpoint)
			result.push_back(MakeDeviceInfo(endpoint.Get(), default_endpoint_id));
	}
	return result;
}

void MusicPlayerLibrary::WasapiExclusiveOutputDevice::ShutdownShared() noexcept
{
	SharedDeviceCache.Shutdown();
}

void MusicPlayerLibrary::WasapiExclusiveOutputDevice::InvalidateShared() noexcept
{
	SharedDeviceCache.Clear();
}

MusicPlayerLibrary::AudioOutputFormat
MusicPlayerLibrary::WasapiExclusiveOutputDevice::ResolveSinkFormat(
	const AudioOutputFormat&) const
{
	return output_format_;
}

IAudioClient*
MusicPlayerLibrary::WasapiExclusiveOutputDevice::ActivateAudioClient() const
{
	const auto enumerator = CreateDeviceEnumerator();
	Microsoft::WRL::ComPtr<IMMDevice> endpoint;
	const HRESULT endpoint_status = enumerator->GetDevice(
		endpoint_id_.c_str(), endpoint.GetAddressOf());
	if (FAILED(endpoint_status) || !endpoint)
		ThrowHResult("IMMDeviceEnumerator::GetDevice", endpoint_status);
	auto client = ActivateEndpointAudioClient(endpoint.Get());
	return client.Detach();
}

std::uint32_t
MusicPlayerLibrary::WasapiExclusiveOutputDevice::GetCurrentLatencyInSamples()
	const noexcept
{
	if (default_device_period_100ns_ <= 0 || output_format_.sample_rate <= 0)
		return 0;
	return WasapiFramesFromReferenceTimeCeiling(
		default_device_period_100ns_, output_format_.sample_rate);
}

MusicPlayerLibrary::WasapiExclusiveOutputDevice::~WasapiExclusiveOutputDevice() =
	default;

#endif
