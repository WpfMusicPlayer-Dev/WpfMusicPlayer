// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Managed/MusicPlayerManaged.h"

#include <msclr/marshal_cppstd.h>
#include <stdexcept>
#include <vector>

#include "Audio/Pipeline/Device/AudioOutputDeviceLifecycle.h"
#include "Audio/Pipeline/Device/Common/FAudioOutputDevice.h"
#if defined(_WIN32)
#include "Audio/Pipeline/Device/Windows/AudioOutputDeviceNotification.h"
#include "Audio/Pipeline/Device/Windows/WasapiExclusiveOutputDevice.h"
#endif

namespace
{
	bool HasAudioFormatInfo(
		const MusicPlayerLibrary::AudioFormatInfo& format) noexcept
	{
		return format.channel_type_id !=
				static_cast<int>(MusicPlayerLibrary::AudioChannelMode::Unknown) ||
			format.sample_rate > 0 ||
			format.bit_depth !=
				static_cast<int>(MusicPlayerLibrary::AudioBitDepth::Unknown);
	}

	System::InvalidOperationException^ ToManagedException(const std::exception& exception)
	{
		const std::string message = exception.what();
		return gcnew System::InvalidOperationException(
			gcnew System::String(message.c_str(), 0, static_cast<int>(message.size()),
				System::Text::Encoding::UTF8));
	}

	System::String^ ToManagedUtf8(const std::string& value)
	{
		return gcnew System::String(
			value.c_str(), 0, static_cast<int>(value.size()),
			System::Text::Encoding::UTF8);
	}
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioChannelType(
	const int channel_type_id)
{
	switch (static_cast<AudioChannelMode>(channel_type_id))
	{
	case AudioChannelMode::System: return "System";
	case AudioChannelMode::Mono: return "Mono";
	case AudioChannelMode::Stereo: return "Stereo";
	case AudioChannelMode::Surround51: return "Surround 5.1";
	case AudioChannelMode::Surround71: return "Surround 7.1";
	case AudioChannelMode::Unknown:
	default:
		return "Unknown channel layout";
	}
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioSampleRate(
	const int sample_rate)
{
	if (sample_rate <= 0)
		return "Unknown sample rate";
	if (sample_rate < 1'000)
	{
		return System::String::Format(
			System::Globalization::CultureInfo::InvariantCulture,
			"{0} Hz", sample_rate);
	}
	return System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0:0.###} kHz", sample_rate / 1'000.0);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioBitDepth(
	const int bit_depth)
{
	if (bit_depth == static_cast<int>(AudioBitDepth::System))
		return "System";
	if (bit_depth < 0)
		return "Unknown bit depth";
	return System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0}-bit", bit_depth);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioFormat(
	const int channel_type_id,
	const int sample_rate,
	const int bit_depth,
	const int bit_rate)
{
	return 
		System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0} / {1} / {2}, {3:f2} kbps",
		FormatAudioSampleRate(sample_rate),
		FormatAudioBitDepth(bit_depth),
		FormatAudioChannelType(channel_type_id),
		bit_rate / 1000.0);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioFormat(int channel_type_id, int sample_rate,
	int bit_depth)
{
	return 
	System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0} / {1} / {2}",
		FormatAudioSampleRate(sample_rate),
		FormatAudioBitDepth(bit_depth),
		FormatAudioChannelType(channel_type_id));
}

void MusicPlayerLibrary::MusicPlayerManaged::GetSystemDefaultOutputFormat(
	int% channel_type_id,
	int% sample_rate,
	int% bit_depth)
{
	channel_type_id = static_cast<int>(AudioChannelMode::Unknown);
	sample_rate = 0;
	bit_depth = static_cast<int>(AudioBitDepth::Unknown);
	try
	{
		const AudioFormatInfo format = GetAudioFormatInfo(ResolveAudioOutputFormat());
		channel_type_id = format.channel_type_id;
		sample_rate = format.sample_rate;
		bit_depth = format.bit_depth;
	}
	catch (const std::exception& exception)
	{
		throw ToManagedException(exception);
	}
}

array<MusicPlayerLibrary::AudioOutputDeviceInfoManaged^>^
MusicPlayerLibrary::MusicPlayerManaged::GetAudioOutputDevices(const int backend)
{
	try
	{
		std::vector<AudioOutputDeviceInfo> devices;
		switch (static_cast<AudioBackend>(backend))
		{
		case AudioBackend::FAudio:
			devices = FAudioOutputDevice::EnumerateOutputDevices();
			break;
#if defined(_WIN32)
		case AudioBackend::WasapiExclusive:
			devices = WasapiExclusiveOutputDevice::EnumerateOutputDevices();
			break;
#endif
		default:
			throw std::invalid_argument("Unsupported audio backend");
		}

		auto result = gcnew array<AudioOutputDeviceInfoManaged^>(
			static_cast<int>(devices.size()));
		for (std::size_t index = 0; index < devices.size(); ++index)
		{
			const auto& device = devices[index];
			result[static_cast<int>(index)] = gcnew AudioOutputDeviceInfoManaged(
				ToManagedUtf8(device.id),
				ToManagedUtf8(device.display_name),
				device.is_default);
		}
		return result;
	}
	catch (const std::exception& exception)
	{
		throw ToManagedException(exception);
	}
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged()
{
	try
	{
		event_bridge = new MusicPlayerEventBridge(this);
		native_handle = new AudioFile(event_bridge);
		initialize_audio_output_device_notifications();
	}
	catch (const std::exception& exception)
	{
		delete event_bridge;
		event_bridge = nullptr;
		native_handle = nullptr;
		throw ToManagedException(exception);
	}
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged(int sample_rate)
{
	try
	{
		event_bridge = new MusicPlayerEventBridge(this);
		AudioOutputFormat requested{};
		requested.requested_sample_rate = sample_rate;
		native_handle = new AudioFile(event_bridge, requested);
		initialize_audio_output_device_notifications();
	}
	catch (const std::exception& exception)
	{
		delete event_bridge;
		event_bridge = nullptr;
		native_handle = nullptr;
		throw ToManagedException(exception);
	}
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged(
	int sample_rate,
	int channel_mode,
	int bit_depth)
{
	try
	{
		event_bridge = new MusicPlayerEventBridge(this);
		AudioOutputFormat requested{};
		requested.requested_sample_rate = sample_rate;
		requested.requested_channel_mode = static_cast<AudioChannelMode>(channel_mode);
		requested.requested_bit_depth = static_cast<AudioBitDepth>(bit_depth);
		native_handle = new AudioFile(event_bridge, requested);
		initialize_audio_output_device_notifications();
	}
	catch (const std::exception& exception)
	{
		delete event_bridge;
		event_bridge = nullptr;
		native_handle = nullptr;
		throw ToManagedException(exception);
	}
}

void MusicPlayerLibrary::MusicPlayerManaged::check_if_null()
{
	if (!native_handle)
		throw gcnew System::InvalidOperationException("MusicPlayerNative initialization failed!");
}

void MusicPlayerLibrary::MusicPlayerManaged::ProcessEvent(int event_type, Object^ payload)
{
	if (!native_handle)
		return; // 析构后或尚未初始化，安静忽略

	ProcessEventState^ state = gcnew ProcessEventState();
	state->EventType = event_type;
	state->Payload = payload;
	switch (static_cast<PlayerMessageType>(event_type))
	{
	case PlayerMessageType::FileInitialized: state->Callback = OnPlayerFileInit; break;
	case PlayerMessageType::AlbumArtInitialized: state->Callback = OnPlayerAlbumArtInit; break;
	case PlayerMessageType::NcmAlbumArtDownloadRequired:
		state->Callback = OnPlayerNcmRequireAlbumArtDownload; break;
	case PlayerMessageType::Started: state->Callback = OnPlayerStart; break;
	case PlayerMessageType::Paused: state->Callback = OnPlayerPause; break;
	case PlayerMessageType::Stopped: state->Callback = OnPlayerStop; break;
	case PlayerMessageType::Destroyed: state->Callback = OnPlayerDestroy; break;
	case PlayerMessageType::Error: state->Callback = OnPlayerError; break;
	case PlayerMessageType::TimeChanged: state->Callback = OnPlayerTimeChange; break;
	case PlayerMessageType::AudioOutputDeviceChanged:
		state->Callback = OnAudioOutputDeviceChanged; break;
	default: state->Callback = nullptr; break;
	}
	event_queue->Enqueue(state);
	if (System::Threading::Interlocked::CompareExchange(
		event_dispatch_scheduled, 1, 0) == 0)
	{
		System::Threading::ThreadPool::QueueUserWorkItem(
			gcnew System::Threading::WaitCallback(
				this, &MusicPlayerManaged::DrainEventQueue));
	}
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged(
	const int sample_rate,
	const int channel_mode,
	const int bit_depth,
	const int backend,
	System::String^ output_device_id)
{
	try
	{
		event_bridge = new MusicPlayerEventBridge(this);
		AudioOutputFormat requested{};
		requested.requested_sample_rate = sample_rate;
		requested.requested_channel_mode = static_cast<AudioChannelMode>(channel_mode);
		requested.requested_bit_depth = static_cast<AudioBitDepth>(bit_depth);
		requested.requested_backend = static_cast<AudioBackend>(backend);
		if (!System::String::IsNullOrEmpty(output_device_id))
		{
			requested.requested_device_id =
				msclr::interop::marshal_as<std::string>(output_device_id);
		}
		native_handle = new AudioFile(event_bridge, requested);
		initialize_audio_output_device_notifications();
	}
	catch (const std::exception& exception)
	{
		delete event_bridge;
		event_bridge = nullptr;
		native_handle = nullptr;
		throw ToManagedException(exception);
	}
}

void MusicPlayerLibrary::MusicPlayerManaged::DrainEventQueue(Object^)
{
	try
	{
		ProcessEventState^ state;
		while (event_queue->TryDequeue(state))
		{
			try
			{
				ProcessEventCore(state);
			}
			catch (System::Exception^ exception)
			{
				System::Diagnostics::Trace::TraceError(
					System::String::Concat(
						"MusicPlayerManaged event callback failed: ",
						exception->ToString()));
			}
		}
	}
	finally
	{
		System::Threading::Interlocked::Exchange(event_dispatch_scheduled, 0);
		if (!event_queue->IsEmpty &&
			System::Threading::Interlocked::CompareExchange(
				event_dispatch_scheduled, 1, 0) == 0)
		{
			System::Threading::ThreadPool::QueueUserWorkItem(
				gcnew System::Threading::WaitCallback(
					this, &MusicPlayerManaged::DrainEventQueue));
		}
	}
}

void MusicPlayerLibrary::MusicPlayerManaged::ProcessEventCore(Object^ stateObj)
{	
	if (!native_handle || !stateObj)
		return; // native 已被销毁，跳过
	ProcessEventState^ state = safe_cast<ProcessEventState^>(stateObj);
	Object^ encoded_payload = state->Payload;

	switch (static_cast<PlayerMessageType>(state->EventType)) {
	case PlayerMessageType::FileInitialized:
		if (state->Callback)
			safe_cast<PlayerFileInitDelegate^>(state->Callback)();
		break;
	case PlayerMessageType::AlbumArtInitialized:
		if (state->Callback)
			safe_cast<PlayerAlbumArtInitDelegate^>(state->Callback)(
				safe_cast<array<System::Byte>^>(encoded_payload));
		break;
	case PlayerMessageType::NcmAlbumArtDownloadRequired:
		if (state->Callback)
			safe_cast<PlayerNcmRequireAlbumArtDownloadDelegate^>(state->Callback)(
				safe_cast<System::String^>(encoded_payload));
		break;
	case PlayerMessageType::Started:
		if (state->Callback)
			safe_cast<PlayerStartDelegate^>(state->Callback)();
		break;
	case PlayerMessageType::Paused:
		if (state->Callback)
			safe_cast<PlayerPauseDelegate^>(state->Callback)();
		break;
	case PlayerMessageType::Stopped:
		if (state->Callback)
			safe_cast<PlayerStopDelegate^>(state->Callback)();
		break;
	case PlayerMessageType::Destroyed:
		if (state->Callback)
			safe_cast<PlayerDestroyDelegate^>(state->Callback)();
		break;
	case PlayerMessageType::Error:
		if (state->Callback)
			safe_cast<PlayerErrorDelegate^>(state->Callback)(
				encoded_payload ? safe_cast<System::Exception^>(encoded_payload) : nullptr);
		break;
	case PlayerMessageType::TimeChanged:
		if (state->Callback) {
			double time = encoded_payload ? safe_cast<double>(state->Payload) : 0.0;
			safe_cast<PlayerTimeChangeDelegate^>(state->Callback)(time);
		}
		break;
	case PlayerMessageType::AudioOutputDeviceChanged:
		if (state->Callback)
			safe_cast<AudioOutputDeviceChangedDelegate^>(state->Callback)();
		break;
	default:
		break;
	}
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsInitialized()
{
	if (!is_native_valid()) return false;
	return native_handle->IsInitialized();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsPlaying()
{
	if (!is_native_valid()) return false;
	return native_handle->IsPlaying();
}

static bool IsValidPath(const std::wstring& path)
{
	if (path.empty())
		return false;

	static constexpr std::wstring_view invalidChars = L"<>\"|?*";
	for (wchar_t invalidChar : invalidChars)
	{
		if (path.find(invalidChar) != std::wstring::npos)
		{
			NATIVE_TRACE("err: invalid character found, char: %c\n", static_cast<char>(invalidChar));
			return false;
		}
	}
	return MusicPlayerLibrary::GetDefaultFileSystem().FileExists(path);
}

void MusicPlayerLibrary::MusicPlayerManaged::OpenFile(const System::String^ fileName)
{
	OpenFile(fileName, false);
}

void MusicPlayerLibrary::MusicPlayerManaged::OpenFile(const System::String^ fileName, bool skipAlbumArtLoading)
{
	check_if_null();
	pin_ptr<const wchar_t> wch = PtrToStringChars(fileName);
	std::wstring nativeFileName(wch);
	if (!IsValidPath(nativeFileName)) {
		throw gcnew System::ArgumentException("file does not exist!");
	}
	auto& default_fs = GetDefaultFileSystem();
	std::wstring extension = default_fs.GetFileExtension(nativeFileName);
	if (!extension.empty() && extension[0] == L'.')
		extension.erase(0, 1);
	try
	{
		native_handle->OpenFile(nativeFileName, extension, skipAlbumArtLoading);
	}
	catch (const std::exception& exception)
	{
		throw ToManagedException(exception);
	}
}

void MusicPlayerLibrary::MusicPlayerManaged::CloseFile()
{
	if (is_native_valid())
		native_handle->Close();
}

double MusicPlayerLibrary::MusicPlayerManaged::GetMusicTimeLength()
{
	if (!is_native_valid()) return 0.0;
	return native_handle->GetMusicTimeLength();
}

double MusicPlayerLibrary::MusicPlayerManaged::GetCurrentMusicPosition()
{
	if (!is_native_valid()) return 0.0;
	return native_handle->GetCurrentMusicPosition();
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetSongTitle()
{
	if (!is_native_valid()) return nullptr;
	std::wstring title = native_handle->GetSongTitle();
	// TODO: 在此处插入 return 语句
	if (title.empty()) return nullptr;
	return gcnew System::String(title.c_str());
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetSongArtist()
{
	if (!is_native_valid()) return nullptr;
	std::wstring artist = native_handle->GetSongArtist();
	// TODO: 在此处插入 return 语句
	if (artist.empty()) return nullptr;
	return gcnew System::String(artist.c_str());
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetAudioSourceFormat()
{
	if (!is_native_valid()) return nullptr;
	const AudioFormatInfo format = native_handle->GetAudioSourceFormatInfo();
	if (!HasAudioFormatInfo(format)) return nullptr;
	return FormatAudioFormat(
		format.channel_type_id, format.sample_rate, format.bit_depth, native_handle->GetAverageAudioBitrate());
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetDeviceOutputFormat()
{
	if (!is_native_valid()) return nullptr;
	const AudioFormatInfo format = native_handle->GetDeviceOutputFormatInfo();
	return FormatAudioFormat(
		format.channel_type_id, format.sample_rate, format.bit_depth);
}

int MusicPlayerLibrary::MusicPlayerManaged::GetActiveAudioBackend()
{
	if (!is_native_valid())
		return static_cast<int>(AudioBackend::FAudio);
	return static_cast<int>(
		native_handle->GetNormalizedFormat().requested_backend);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetActiveOutputDeviceId()
{
	if (!is_native_valid())
		return System::String::Empty;
	return ToManagedUtf8(
		native_handle->GetNormalizedFormat().requested_device_id);
}

int MusicPlayerLibrary::MusicPlayerManaged::GetDeviceOutputChannelType()
{
	if (!is_native_valid())
		return static_cast<int>(AudioChannelMode::Unknown);
	return native_handle->GetDeviceOutputFormatInfo().channel_type_id;
}

int MusicPlayerLibrary::MusicPlayerManaged::GetDeviceOutputSampleRate()
{
	if (!is_native_valid()) return 0;
	return native_handle->GetDeviceOutputFormatInfo().sample_rate;
}

int MusicPlayerLibrary::MusicPlayerManaged::GetDeviceOutputBitDepth()
{
	if (!is_native_valid())
		return static_cast<int>(AudioBitDepth::Unknown);
	return native_handle->GetDeviceOutputFormatInfo().bit_depth;
}

void MusicPlayerLibrary::MusicPlayerManaged::InvalidateAudioOutputDeviceCaches()
{
	MusicPlayerLibrary::InvalidateAudioOutputDevices();
}

System::UInt64 MusicPlayerLibrary::MusicPlayerManaged::
GetAudioOutputDeviceChangeRevision()
{
#if defined(_WIN32)
	return MusicPlayerLibrary::GetAudioOutputDeviceChangeRevision();
#else
	return 0;
#endif
}

double MusicPlayerLibrary::MusicPlayerManaged::GetAudioSourceBitrate()
{
	if (!is_native_valid()) return 0.0;
	return native_handle->GetAudioSourceBitrate();
}

int MusicPlayerLibrary::MusicPlayerManaged::GetAverageAudioBitrate()
{
	if (!is_native_valid()) return 0;
	return native_handle->GetAverageAudioBitrate();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsLoselessAudio()
{
	if (!is_native_valid()) return false;
	return native_handle->IsLoselessAudio();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsHiResAudio()
{
	if (!is_native_valid()) return false;
	return native_handle->IsHiResAudio();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsBitPerfect()
{
	if (!is_native_valid()) return false;
	return native_handle->IsBitPerfect();
}

void MusicPlayerLibrary::MusicPlayerManaged::Start()
{
	check_if_null();
	native_handle->Start();
}

void MusicPlayerLibrary::MusicPlayerManaged::Pause()
{
	check_if_null();
	native_handle->Pause();
}

void MusicPlayerLibrary::MusicPlayerManaged::Stop()
{
	check_if_null();
	native_handle->Stop();
}

void MusicPlayerLibrary::MusicPlayerManaged::SetMasterVolume(float volume)
{
	check_if_null();
	native_handle->SetMasterVolume(volume);
}

void MusicPlayerLibrary::MusicPlayerManaged::SeekToPosition(double time, bool need_stop)
{
	check_if_null();
	native_handle->SeekToPosition(time, need_stop);
}

int MusicPlayerLibrary::MusicPlayerManaged::GetNBlockAlign()
{
	if (!is_native_valid()) return -1;
	return native_handle->GetNBlockAlign();
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetID3Lyric()
{
	if (!is_native_valid()) return nullptr;
	std::wstring lyric = native_handle->GetID3Lyric();
	// TODO: 在此处插入 return 语句
	return gcnew System::String(lyric.c_str());
}

int MusicPlayerLibrary::MusicPlayerManaged::GetEqualizerBand(int index)
{
	if (!is_native_valid()) return 0;
	return native_handle->GetEqualizerBand(index);
}

void MusicPlayerLibrary::MusicPlayerManaged::SetEqualizerBand(int index, int value)
{
	check_if_null();
	native_handle->SetEqualizerBand(index, value);
}

MusicPlayerLibrary::MusicPlayerManaged::~MusicPlayerManaged()
{
	release_native_resources();
	OnPlayerFileInit = nullptr;
	OnPlayerAlbumArtInit = nullptr;
	OnPlayerStart = nullptr;
	OnPlayerPause = nullptr;
	OnPlayerStop = nullptr;
	OnPlayerTimeChange = nullptr;
	OnPlayerDestroy = nullptr;
	OnPlayerError = nullptr;
	OnPlayerNcmRequireAlbumArtDownload = nullptr;
	OnAudioOutputDeviceChanged = nullptr;
	System::GC::SuppressFinalize(this);
}

void MusicPlayerLibrary::MusicPlayerManaged::!MusicPlayerManaged()
{
	release_native_resources();
	OnPlayerFileInit = nullptr;
	OnPlayerAlbumArtInit = nullptr;
	OnPlayerStart = nullptr;
	OnPlayerPause = nullptr;
	OnPlayerStop = nullptr;
	OnPlayerTimeChange = nullptr;
	OnPlayerDestroy = nullptr;
	OnPlayerError = nullptr;
	OnPlayerNcmRequireAlbumArtDownload = nullptr;
	OnAudioOutputDeviceChanged = nullptr;
}

void MusicPlayerLibrary::MusicPlayerManaged::release_native_resources()
{
#if defined(_WIN32)
	delete audio_output_device_change_subscription;
	audio_output_device_change_subscription = nullptr;
#endif
	AudioFile* handle = native_handle;
	native_handle = nullptr;
	delete handle;
	delete event_bridge;
	event_bridge = nullptr;
}

void MusicPlayerLibrary::MusicPlayerManaged::
	initialize_audio_output_device_notifications()
{
#if defined(_WIN32)
	try
	{
		auto subscription = SubscribeAudioOutputDeviceChanges(event_bridge);
		audio_output_device_change_subscription = subscription.release();
		if (!audio_output_device_change_subscription)
		{
			NATIVE_TRACE(
				"warn: WASAPI output device notifications are unavailable\n");
		}
	}
	catch (...)
	{
		audio_output_device_change_subscription = nullptr;
		NATIVE_TRACE(
			"warn: failed to subscribe to WASAPI output device notifications\n");
	}
#endif
}

int MusicPlayerLibrary::MusicPlayerManaged::CopyAudioFFTData(array<float>^ destination)
{
	if (!is_native_valid())
		return 0;
	if (destination == nullptr || destination->Length <= 0)
		return 0;

	pin_ptr<float> destination_ptr = &destination[0];
	return native_handle->CopyAudioFFTData(destination_ptr, destination->Length);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetSharedDeviceOutputFormat()
{
	if (!is_native_valid()) return nullptr;
	const AudioFormatInfo format = native_handle->GetSharedDeviceOutputFormatInfo();
	return FormatAudioFormat(
		format.channel_type_id, format.sample_rate, format.bit_depth);
}
