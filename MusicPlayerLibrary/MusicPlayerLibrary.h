#pragma once

#include "AtlTraceRedirect.h"
#include <vcclr.h>
using namespace System;

namespace MusicPlayerLibrary {

	public ref class AtlTraceRedirectManager {
		static AtlTraceRedirect* m_pRedirector;
		public:
			static void Init();

	};

	public delegate void WriteRawPCMBytesCallback(array<uint8_t>^ buffer_out, int buffer_size);

	public enum audio_playback_state : unsigned long long
	{
		audio_playback_state_init,
		audio_playback_state_playing,
		audio_playback_state_paused,
		audio_playback_state_decoder_exit_pre_stop,
		audio_playback_state_stopped
	};

	public enum MessageType : UINT {
		WM_PLAYER_FILE_INIT = (WM_USER + 100),
		WM_PLAYER_TIME_CHANGE = (WM_USER + 101),
		WM_PLAYER_START = (WM_USER + 102),
		WM_PLAYER_PAUSE = (WM_USER + 103),
		WM_PLAYER_STOP = (WM_USER + 104),
		WM_PLAYER_ALBUM_ART_INIT = (WM_USER + 105),
		WM_PLAYER_DESTROY = (WM_USER + 106)
	};

	public ref class Class1
	{
		// TODO: 在此处为此类添加方法。
		public:
			int Add(int a, int b);
	};

	public struct NcmMusicMeta
	{
		CString musicName;
		std::vector<std::vector<CString>> artist;
		CString format;
		CString album;
		CString albumPic;
	};

	public struct DecryptResult
	{
		CString title;
		CString artist;
		CString album;
		CString ext;
		CString pictureUrl;
		std::vector<uint8_t> audioData;
		CString mime;
	};

	public class NcmDecryptor
	{
	public:
		NcmDecryptor(const std::vector<uint8_t>& data, const CString& filename);
		DecryptResult Decrypt();

	private:
		const std::vector<uint8_t>& m_raw;
		size_t m_offset = 0;
		CString m_filename;

		NcmMusicMeta m_oriMeta;
		std::vector<uint8_t> m_audio;
		CString m_format;
		CString m_mime;

		std::vector<uint8_t> GetKeyData();
		std::vector<uint8_t> GetKeyBox();
		NcmMusicMeta GetMetaData();
		std::vector<uint8_t> GetAudio(const std::vector<uint8_t>& keyBox);

		static CString Utf8ToWstring(const CString& s);
	};

	public class CriticalSectionLock
	{
		LPCRITICAL_SECTION cs;
	public:
		explicit CriticalSectionLock(LPCRITICAL_SECTION section, bool spinwait = false) : cs(section) {
			if (spinwait)
				while (!TryEnterCriticalSection(cs)) {}
			else
				EnterCriticalSection(cs);
		}
		~CriticalSectionLock() { LeaveCriticalSection(cs); }

		CriticalSectionLock(const CriticalSectionLock&) = delete;
		CriticalSectionLock& operator=(const CriticalSectionLock&) = delete;
		CriticalSectionLock(CriticalSectionLock&&) = delete;
		CriticalSectionLock& operator=(CriticalSectionLock&&) = delete;
	};

	ref class MusicPlayer;
	public class MusicPlayerNative
	{
		// 流文件解析上下文
		AVFormatContext* format_context = nullptr;
		// 针对该文件，找到的解码器类型
		AVCodec* codec = nullptr;
		// 使用的解码器实例
		AVCodecContext* codec_context = nullptr;
		// 解码前的数据（流中的一个packet）
		AVPacket* packet = nullptr;
		// 解码后的数据（一帧数据）
		AVFrame* frame = nullptr;
		AVFrame* filt_frame = nullptr;
		// 音频流编号
		unsigned audio_stream_index = static_cast<unsigned>(-1); // inf
		AVIOContext* avio_context = nullptr;
		unsigned char* buffer = nullptr;

		CString file_extension;
		CFile* file_stream = nullptr;
		bool file_stream_end = false;
		bool user_request_stop = false;
		double pts_seconds = 0.0;
		float elapsed_time = 0.0;
		float length = 0.0f;
		bool is_pause = false;
		bool decoder_is_running = false;
		int decoder_audio_channels = 0;
		AVSampleFormat decoder_audio_sample_fmt = AV_SAMPLE_FMT_NONE;
		HBITMAP album_art = nullptr;
		CString song_title = {};
		CString song_artist = {};

		CRITICAL_SECTION* audio_fifo_section{};
		CRITICAL_SECTION* audio_playback_section;

		IXAudio2* xaudio2 = nullptr;
		IXAudio2MasteringVoice* mastering_voice = nullptr;
		IXAudio2SourceVoice* source_voice = nullptr;
		SwrContext* swr_ctx = nullptr;
		WAVEFORMATEX wfx = {};

		volatile unsigned long long* xaudio2_buffer_ended;
		volatile unsigned long long* playback_state;
		volatile unsigned long long* audio_position;
		CWinThread* audio_player_worker_thread = nullptr;
		CWinThread* audio_decoder_worker_thread = nullptr;

		std::list<XAUDIO2_BUFFER*> xaudio2_playing_buffers = {};
		std::list<XAUDIO2_BUFFER*> xaudio2_free_buffers = {};
		size_t xaudio2_played_buffers = 0, xaudio2_allocated_buffers = 0, xaudio2_played_samples = 0;
		uint8_t* out_buffer = nullptr;
		size_t out_buffer_size = 0, base_offset = 0;

		// use avaudiofifo to avoid lag on low-cpu performance system, like jasper lake/alder lake-n
		AVAudioFifo* audio_fifo = nullptr;
		int xaudio2_play_frame_size = 256;
		LPDWORD xaudio2_thread_task_index = nullptr;
		HANDLE frame_ready_event = nullptr;
		HANDLE frame_underrun_event = nullptr;
		double standard_frametime = 0.0, last_frametime = 0.0;
		float message_interval = 16.67f, message_interval_timer = 0.0f;
		size_t prev_decode_cycle_xaudio2_played_samples = 0;
		CString id3_string_lyric;

		// managed variables
		gcroot<MusicPlayer^> managed_music_player;

		// file I/O Area
		int read_func(uint8_t* buf, int buf_size);
		static int read_func_wrapper(void* opaque, uint8_t* buf, int buf_size);
		int64_t seek_func(int64_t offset, int whence);
		static int64_t seek_func_wrapper(void* opaque, int64_t offset, int whence);
		int load_audio_context(const CString& audio_filename, const CString& file_extension_in = CString());
		int load_audio_context_stream(CFile* in_file_stream);
		void release_audio_context();
		void reset_audio_context();
		bool is_audio_context_initialized();
		static HBITMAP download_ncm_album_art(const CString& url, int scale_size = 128);
		HBITMAP decode_id3_album_art(int stream_index, int scale_size = 128);
		void download_ncm_album_art_async(const CString& url, int scale_size);
		void read_metadata();

		// playback area
		int initialize_audio_engine();
		void init_decoder_thread();
		void uninitialize_audio_engine();
		void audio_playback_worker_thread();
		void audio_decode_worker_thread();
		void start_audio_playback();
		void stop_audio_decode(int mode = 0);
		void stop_audio_playback(int mode);

		int initialize_audio_fifo(AVSampleFormat sample_fmt, int channels, int nb_samples);
		int resize_audio_fifo(int nb_samples);
		int add_samples_to_fifo(uint8_t** decoded_data, int nb_samples);
		int read_samples_from_fifo(uint8_t** output_buffer, int nb_samples);
		void drain_audio_fifo(int nb_samples);
		void reset_audio_fifo();
		int get_audio_fifo_cached_samples_size();
		void uninitialize_audio_fifo();

		// XAudio2 helper function
		const char* get_backend_implement_version();
		void xaudio2_init_buffer(XAUDIO2_BUFFER* dest_buffer, int size = 8192);
		XAUDIO2_BUFFER* xaudio2_allocate_buffer(int size = 8192);
		XAUDIO2_BUFFER* xaudio2_get_available_buffer(int size = 8192);
		void xaudio2_free_buffer();
		void xaudio2_destroy_buffer();
		int decoder_query_xaudio2_buffer_size();
		bool is_xaudio2_initialized();
		size_t get_samples_played_per_session();
	public:
		// using WriteRawPCMBytesCallback = std::function<void(const uint8_t* buffer_out, int buffer_size)>;
	protected:
		gcroot<WriteRawPCMBytesCallback^> write_raw_pcm_bytes_callback;

		// debug function
		void dialog_ffmpeg_critical_error(int err_code, const char* file, int line);

		// equalizer settings
		CSimpleArray<int> eq_bands;
		AVFilterGraph* filter_graph = nullptr;
		struct av_filter_eq_graph
		{
			int freq;
			int gain_values;
			AVFilterContext* eq_context;
			CStringA eq_name;
		};
		AVFilterContext* filter_context_src = nullptr, * filter_context_sink = nullptr, * channels_normalize_ctx = nullptr,
			* volume_ctx = nullptr, * limiter_ctx = nullptr, * format_normalize_ctx = nullptr;
		CSimpleArray<av_filter_eq_graph> filter_graphs;

		void init_av_filter_equalizer();
		bool is_av_filter_equalizer_initialized();
		void reset_av_filter_equalizer();
	public:
		volatile bool suppress_time_events = false;

		// constructor
		MusicPlayerNative();
		// no copy & move
		MusicPlayerNative(const MusicPlayerNative&) = delete;
		MusicPlayerNative& operator=(const MusicPlayerNative&) = delete;
		MusicPlayerNative(MusicPlayerNative&&) = delete;
		MusicPlayerNative& operator=(MusicPlayerNative&&) = delete;

		// Interfaces
		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const CString& fileName, const CString& file_extension_in = CString());
		float GetMusicTimeLength();
		float GetCurrentMusicPosition();
		CString GetSongTitle();
		CString GetSongArtist();
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(float time, bool need_stop);
		// int GetRawPCMBytes(uint8_t* buffer_out, int buffer_size) const;

		// WritePCMBytesCallback:
		// uint8_t* out: provided by MusicPlayer, out_buffer
		// int buffer_size: provided by MusicPlayer, actual written bytes
		void RegisterWritePCMBytesCallback(WriteRawPCMBytesCallback^ callback);
		void ClearWritePCMBytesCallback();
		int GetNBlockAlign();
		CString GetID3Lyric();

		// Equalizer interfaces
		int GetEqualizerBand(int index);
		void SetEqualizerBand(int index, int value);

		// Managed C++ Interface
		void SetManagedPlayer(MusicPlayer^);

		// destructor
		~MusicPlayerNative();
	};

	public delegate void PlayerFileInitDelegate();
	public delegate void PlayerAlbumArtInitDelegate(System::Drawing::Image^ fromDecode);
	public delegate void PlayerStartDelegate();
	public delegate void PlayerPauseDelegate();
	public delegate void PlayerStopDelegate();
	public delegate void PlayerTimeChangeDelegate(float time);
	public delegate void PlayerDestroyDelegate();

	public ref class MusicPlayer:
		System::ICloneable, System::IDisposable
	{
		MusicPlayerNative* native_handle;

	public:
		property PlayerFileInitDelegate^ OnPlayerFileInit;
		property PlayerAlbumArtInitDelegate^ OnPlayerAlbumArtInit;
		property PlayerStartDelegate^ OnPlayerStart;
		property PlayerPauseDelegate^ OnPlayerPause;
		property PlayerStopDelegate^ OnPlayerStop;
		property PlayerTimeChangeDelegate^ OnPlayerTimeChange;
		property PlayerDestroyDelegate^ OnPlayerDestroy;
	public:
		MusicPlayer();

	private:
		void check_if_null();

		ref class ProcessEventState {
		public:
			MessageType EventType;
			IntPtr WParam;
			IntPtr LParam;
		};
		void ProcessEventCore(Object^ state);

	public:

		/*
		* ProcessEvent is called by native code to notify managed code of various events, such as file initialization, album art initialization, playback start/pause/stop, and time change.
		* Notice: this function is dispatched asynchronously via ThreadPool to avoid deadlocks between native audio threads and the UI/managed thread.
		* Callback functions should NOT perform any heavy operation to avoid blocking the audio thread, which may cause audio stutter.
		* Invoke or similar mechanism to avoid cross-thread operation exceptions.
		*/
		void ProcessEvent(MessageType event_type, WPARAM wParam, LPARAM lParam);

		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const System::String^ fileName);
		float GetMusicTimeLength();
		float GetCurrentMusicPosition();
		System::String^ GetSongTitle();
		System::String^ GetSongArtist();
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(float time, bool need_stop);
		// int GetRawPCMBytes(uint8_t* buffer_out, int buffer_size) const;

		// WritePCMBytesCallback:
		// uint8_t* out: provided by MusicPlayer, out_buffer
		// int buffer_size: provided by MusicPlayer, actual written bytes
		void RegisterWritePCMBytesCallback(WriteRawPCMBytesCallback^ callback);
		void ClearWritePCMBytesCallback();
		int GetNBlockAlign();
		System::String^ GetID3Lyric();

		// Equalizer interfaces
		int GetEqualizerBand(int index);
		void SetEqualizerBand(int index, int value);

		virtual Object^ Clone() {
			throw gcnew System::NotSupportedException("This object cannot be cloned.");
		}

		~MusicPlayer() {
			delete native_handle;
			 native_handle = nullptr;
			 OnPlayerFileInit = nullptr;
			 OnPlayerAlbumArtInit = nullptr;
			 OnPlayerStart = nullptr;
			 OnPlayerPause = nullptr;
			 OnPlayerStop = nullptr;
			 OnPlayerTimeChange = nullptr;
			 OnPlayerDestroy = nullptr;
		}
	};

}
