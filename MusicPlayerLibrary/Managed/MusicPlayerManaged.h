// SPDX-License-Identifier: MIT

#pragma once

#include <vcclr.h>
#include "Audio/MusicPlayerLibrary.h"
#include "Managed/MusicPlayerEventBridge.h"

namespace MusicPlayerLibrary
{
	public ref class AudioOutputDeviceInfoManaged sealed
	{
	public:
		AudioOutputDeviceInfoManaged(
			System::String^ id,
			System::String^ display_name,
			bool is_default)
		{
			Id = id;
			DisplayName = display_name;
			IsDefault = is_default;
		}

		property System::String^ Id;
		property System::String^ DisplayName;
		property bool IsDefault;
	};
    
	public delegate void PlayerFileInitDelegate();
	public delegate void PlayerAlbumArtInitDelegate(array<System::Byte>^ encodedImage);
	public delegate void PlayerStartDelegate();
	public delegate void PlayerPauseDelegate();
	public delegate void PlayerStopDelegate();
	public delegate void PlayerTimeChangeDelegate(double time);
	public delegate void PlayerDestroyDelegate();
	public delegate void PlayerErrorDelegate(System::Exception^ exception);
	public delegate void PlayerNcmRequireAlbumArtDownloadDelegate(System::String^ url);

	public ref class MusicPlayerManaged:
		System::ICloneable, System::IDisposable
	{
		AudioFile* native_handle = nullptr;
		MusicPlayerEventBridge* event_bridge = nullptr;

	public:
		property PlayerFileInitDelegate^ OnPlayerFileInit;
		property PlayerAlbumArtInitDelegate^ OnPlayerAlbumArtInit;
		property PlayerStartDelegate^ OnPlayerStart;
		property PlayerPauseDelegate^ OnPlayerPause;
		property PlayerStopDelegate^ OnPlayerStop;
		property PlayerTimeChangeDelegate^ OnPlayerTimeChange;
		property PlayerDestroyDelegate^ OnPlayerDestroy;
		property PlayerErrorDelegate^ OnPlayerError;
		property PlayerNcmRequireAlbumArtDownloadDelegate^ OnPlayerNcmRequireAlbumArtDownload;
		
		MusicPlayerManaged();
		MusicPlayerManaged(int sample_rate);
		MusicPlayerManaged(int sample_rate, int channel_mode, int bit_depth);
		MusicPlayerManaged(
			int sample_rate,
			int channel_mode,
			int bit_depth,
			int backend,
			System::String^ output_device_id);

	private:
		void check_if_null();
		bool is_native_valid() { return native_handle != nullptr; }

		ref class ProcessEventState {
		public:
			int EventType;
			Object^ Payload;
			Object^ Callback;
		};
		System::Collections::Concurrent::ConcurrentQueue<ProcessEventState^>^
			event_queue = gcnew System::Collections::Concurrent::ConcurrentQueue<ProcessEventState^>();
		int event_dispatch_scheduled = 0;
		void ProcessEventCore(Object^ state);
		void DrainEventQueue(Object^ state);
		void release_native_resources();

		/*
		* ProcessEvent is called by native code to notify managed code of various events, such as file initialization, album art initialization, playback start/pause/stop, and time change.
		* Notice: this function is dispatched asynchronously via ThreadPool to avoid deadlocks between native audio threads and the UI/managed thread.
		* Callback functions should NOT perform any heavy operation to avoid blocking the audio thread, which may cause audio stutter.
		* Invoke or similar mechanism to avoid cross-thread operation exceptions.
		*/
	internal:
		void ProcessEvent(int event_type, Object^ payload);

	public:

		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const System::String^ fileName);
		void OpenFile(const System::String^ fileName, bool skipAlbumArtLoading);
		void CloseFile();
		double GetMusicTimeLength();
		double GetCurrentMusicPosition();
		System::String^ GetSongTitle();
		System::String^ GetSongArtist();
		System::String^ GetAudioSourceFormat();
		System::String^ GetDeviceOutputFormat();
		int GetDeviceOutputSampleRate();
		System::String^ GetSharedDeviceOutputFormat();
		static void GetSystemDefaultOutputFormat(
			[System::Runtime::InteropServices::OutAttribute] int% channel_type_id,
			[System::Runtime::InteropServices::OutAttribute] int% sample_rate,
			[System::Runtime::InteropServices::OutAttribute] int% bit_depth);
		static array<AudioOutputDeviceInfoManaged^>^ GetAudioOutputDevices(
			int backend);
		static System::String^ FormatAudioChannelType(int channel_type_id);
		static System::String^ FormatAudioSampleRate(int sample_rate);
		static System::String^ FormatAudioBitDepth(int bit_depth);
		static System::String^ FormatAudioFormat(
			int channel_type_id,
			int sample_rate,
			int bit_depth,
			int bit_rate);
		
		static System::String^ FormatAudioFormat(
			int channel_type_id,
			int sample_rate,
			int bit_depth);
		double GetAudioSourceBitrate();
		int GetAverageAudioBitrate();
		bool IsLoselessAudio();
		bool IsHiResAudio();
		bool IsBitPerfect();
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(double time, bool need_stop);
		// int GetRawPCMBytes(uint8_t* buffer_out, int buffer_size) const;

		int GetNBlockAlign();
		System::String^ GetID3Lyric();

		// FFT spectrum data
		int CopyAudioFFTData(array<float>^ destination);

		// Equalizer interfaces
		int GetEqualizerBand(int index);
		void SetEqualizerBand(int index, int value);

		virtual Object^ Clone() {
			throw gcnew System::NotSupportedException("This object cannot be cloned.");
		}

		~MusicPlayerManaged();

		!MusicPlayerManaged();
	};

}
