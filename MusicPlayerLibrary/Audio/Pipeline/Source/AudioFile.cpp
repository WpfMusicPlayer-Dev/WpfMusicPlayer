// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string_view>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/opt.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#if defined(__cplusplus)
}
#endif

#include "Crypto/NcmDecryptor.h"
#include "Crypto/NcmFormat.h"
#include "Audio/FFmpeg/FFmpegError.h"
#include "Audio/Pipeline/Source/AudioFile.h"
#include "Audio/Pipeline/Observer/FFTAudioObserve.h"
#include "Audio/Pipeline/Sink/AudioSinkFactory.h"
#include "Core/LocaleConverter.h"
#include "Core/AudioThreadScheduleHelper.h"
#include "Core/ChronoUtilities.h"
#include "Core/FileIO.h"
#include "Core/StringUtilities.h"

#if !defined(FFMPEG_CRITICAL_ERROR)
#define FFMPEG_CRITICAL_ERROR(err_code) \
do { \
dialog_ffmpeg_critical_error(err_code, __FILE__, __LINE__); \
} while(0)
#endif

namespace
{
	constexpr auto PipelineBufferGrowthCooldown = std::chrono::seconds(2);
	constexpr auto AudioWorkerPollInterval = std::chrono::milliseconds(2);
	constexpr auto PlaybackPollInterval = std::chrono::milliseconds(1);

	int SamplesForMilliseconds(const int sample_rate, const int milliseconds) noexcept
	{
		const int valid_sample_rate = sample_rate > 0
			? sample_rate
			: MusicPlayerLibrary::StandardAudioSampleRate;
		return static_cast<int>(
			static_cast<std::int64_t>(valid_sample_rate) * milliseconds / 1'000);
	}

	double ElapsedMilliseconds(
		const std::chrono::steady_clock::duration elapsed) noexcept
	{
		return std::chrono::duration<double, std::milli>(elapsed).count();
	}

	int GetEffectiveSourceBitDepth(
		const AVCodecContext& codec_context,
		const AVCodecParameters& codec_parameters) noexcept
	{
		const int reported_depths[] = {
			codec_context.bits_per_raw_sample,
			codec_parameters.bits_per_raw_sample,
			codec_context.bits_per_coded_sample,
			codec_parameters.bits_per_coded_sample
		};
		for (const int bit_depth : reported_depths)
		{
			if (bit_depth > 0)
				return bit_depth;
		}

		const int bytes_per_sample = av_get_bytes_per_sample(codec_context.sample_fmt);
		return bytes_per_sample > 0 ? bytes_per_sample * 8 : 0;
	}

	std::uint64_t GetNativeChannelMask(
		const AVChannelLayout& channel_layout) noexcept
	{
		return channel_layout.order == AV_CHANNEL_ORDER_NATIVE
			? channel_layout.u.mask
			: 0;
	}

	MusicPlayerLibrary::DecodedAudioFormat MakeDecodedAudioFormat(
		const int sample_rate,
		const AVSampleFormat sample_format,
		const AVChannelLayout& channel_layout,
		const int bit_depth) noexcept
	{
		std::uint64_t channel_mask = GetNativeChannelMask(channel_layout);
		// Legacy mono/stereo containers (notably RIFF/WAVEFORMATEX) carry only
		// a channel count. Their order is nevertheless unambiguous and FFmpeg
		// treats it as the standard mono/stereo layout; canonicalize that implicit
		// contract before the exact sink comparison. Wider unspecified layouts
		// remain conservative and require normalization.
		if (channel_mask == 0 &&
			channel_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		{
			if (channel_layout.nb_channels == 1)
				channel_mask = AV_CH_LAYOUT_MONO;
			else if (channel_layout.nb_channels == 2)
				channel_mask = AV_CH_LAYOUT_STEREO;
		}

		return {
			.sample_rate = sample_rate,
			.channel_count = channel_layout.nb_channels > 0
				? static_cast<std::uint16_t>(channel_layout.nb_channels)
				: std::uint16_t{0},
			.channel_mask = channel_mask,
			.bit_depth = bit_depth,
			.sample_format = sample_format
		};
	}

	double GetAudioStreamDurationSeconds(
		const AVFormatContext* format_context,
		const int audio_stream_index) noexcept
	{
		if (!format_context || audio_stream_index < 0 ||
			static_cast<unsigned int>(audio_stream_index) >= format_context->nb_streams)
		{
			return 0.0;
		}

		const AVStream* audio_stream = format_context->streams[audio_stream_index];
		if (audio_stream && audio_stream->duration > 0 &&
			audio_stream->duration != AV_NOPTS_VALUE && audio_stream->time_base.den != 0)
		{
			const double duration_seconds =
				static_cast<double>(audio_stream->duration) * av_q2d(audio_stream->time_base);
			if (std::isfinite(duration_seconds) && duration_seconds > 0.0)
				return duration_seconds;
		}

		if (format_context->duration > 0 && format_context->duration != AV_NOPTS_VALUE)
		{
			const double duration_seconds =
				static_cast<double>(format_context->duration) / AV_TIME_BASE;
			if (std::isfinite(duration_seconds) && duration_seconds > 0.0)
				return duration_seconds;
		}
		return 0.0;
	}
}

int MusicPlayerLibrary::AudioFile::read_func(uint8_t* buf, int buf_size) {
	// ATLTRACE("info: read buf_size=%d, rest=%lld\n", buf_size, file_stream->GetLength() - file_stream->GetPosition());
	// reset file_stream_end
	file_stream_end = false;
	int gcount = static_cast<int>(file_stream->Read(buf, buf_size));
	if (gcount == 0) {
		file_stream_end = true;
		return AVERROR_EOF;
	}
	return gcount;
}

int MusicPlayerLibrary::AudioFile::read_func_wrapper(void* opaque, uint8_t* buf, int buf_size)
{
	auto callObject = reinterpret_cast<AudioFile*>(opaque);
	return callObject->read_func(buf, buf_size);
}

int64_t MusicPlayerLibrary::AudioFile::seek_func(int64_t offset, int whence)
{
	FileSeekOrigin origin;
	switch (whence) {
	case AVSEEK_SIZE: return static_cast<int64_t>(file_stream->GetLength());
	case SEEK_SET: origin = FileSeekOrigin::Begin; break;
	case SEEK_CUR: origin = FileSeekOrigin::Current; break;
	case SEEK_END: origin = FileSeekOrigin::End; break;
	default: return AVERROR(EINVAL); // unsupported
	}
	uint64_t pos = file_stream->Seek(offset, origin);
	return static_cast<int64_t>(pos);
}

int64_t MusicPlayerLibrary::AudioFile::seek_func_wrapper(void* opaque, int64_t offset, int whence)
{
	auto callObject = reinterpret_cast<AudioFile*>(opaque);
	return callObject->seek_func(offset, whence);
}

inline int MusicPlayerLibrary::AudioFile::load_audio_context(const std::wstring& audio_filename, const std::wstring& file_extension_in, bool skip_album_art_loading)
{
	normalized_stream_end = false;
	is_audio_file_and_sink_bit_perfect.store(false, std::memory_order_relaxed);
	song_title.clear();
	song_artist.clear();
	id3_string_lyric.clear();
	audio_source_format_info = {};
	reset_audio_source_bitrate();
	reset_audio_quality_flags();
	length.store(0.0, std::memory_order_relaxed);
	// 打开文件流
	// std::ios::sync_with_stdio(false);
	file_stream = GetDefaultFileSystem().OpenReadFile(audio_filename, true, false);
	bool is_ncm = false;
	file_extension = file_extension_in;
	this->skip_album_art_loading = skip_album_art_loading;
	if (!file_stream)
	{
		NATIVE_TRACE("err: file not exists!\n");
		return -1;
	}
	std::array<char, NcmMagicHeader.size() + 1> magic{};
	if (!ReadExact(*file_stream, magic.data(), NcmMagicHeader.size()))
	{
		NATIVE_TRACE("err: failed to read magic bytes\n");
		file_stream.reset();
		return -1;
	}
	NATIVE_TRACE("info: magic bytes: %s\n", magic.data());
	if (IsNcmMagicHeader(magic))
	{
		NATIVE_TRACE("info: found ncm header\n");
		is_ncm = true;
	}
	file_stream->SeekToBegin();

	if (EqualsAsciiIgnoreCase<wchar_t>(file_extension_in, L"ncm") || is_ncm)
	{
		try
		{
			file_stream->SeekToBegin();
			auto decryptor_result = NcmDecryptor::Open(std::move(file_stream));
			file_stream = std::move(decryptor_result.audio_file);
			file_stream->SeekToBegin();
			if (!skip_album_art_loading)
			{
				require_download_ncm_album_art(decryptor_result.metadata.pictureUrl);
			}
		}
		catch (std::exception& e)
		{
			NATIVE_TRACE("err: decrypt ncm failed: %s\n", e.what());
			NATIVE_TRACE("err: this can be caused by ncm algorithm update, or ncm file corrupt\n");
			NATIVE_TRACE("err: please try to report ncm file to issues\n");
			file_stream.reset();
			return -1;
		}
		// file_stream now exposes decrypted audio bytes without materializing an output file.
	}
	return load_audio_context_from_file_stream();
}

int MusicPlayerLibrary::AudioFile::load_audio_context_from_file_stream()
{
	if (!file_stream)
		return -1;

	// 重置文件流指针，防止读取后未复位
	file_stream->SeekToBegin();
	auto fail_load = [this]() {
		release_audio_session_resources();
		return -1;
	};

	// 取得文件大小
	format_context = avformat_alloc_context();
	if (!format_context)
	{
		NATIVE_TRACE("err: avformat_alloc_context failed\n");
		return fail_load();
	}
	size_t file_len = static_cast<int64_t>(file_stream->GetLength());
	NATIVE_TRACE("info: file loaded, size = %zu\n", file_len);

	constexpr size_t avio_buf_size = 8192;


	buffer = reinterpret_cast<unsigned char*>(av_malloc(avio_buf_size));
	if (!buffer)
	{
		NATIVE_TRACE("err: av_malloc failed for avio buffer\n");
		return fail_load();
	}
	avio_context =
		avio_alloc_context(buffer, avio_buf_size, 0,
			this,
			&read_func_wrapper,
			nullptr,
			&seek_func_wrapper);
	if (!avio_context)
	{
		NATIVE_TRACE("err: avio_alloc_context failed\n");
		return fail_load();
	}

	format_context->pb = avio_context;

	// 打开音频文件
	int res = avformat_open_input(&format_context,
		nullptr, // dummy parameter, read from memory stream
		nullptr, // let ffmpeg auto detect format
		nullptr  // no parateter specified
	);
	if (res < 0) {
		const auto reason = GetFFmpegErrorMessage(res);
		NATIVE_TRACE("err: avformat_open_input failed: %s\n", reason.c_str());
		return fail_load();
	}
	if (!format_context)
	{
		const auto reason = GetFFmpegErrorMessage(res);
		NATIVE_TRACE("err: avformat_open_input failed, reason = %s(%d)\n",
			reason.c_str(), res);
		return fail_load();
	}

	res = avformat_find_stream_info(format_context, nullptr);
	if (res == AVERROR_EOF)
	{
		NATIVE_TRACE("err: no stream found in file\n");
		return fail_load();
	}
	NATIVE_TRACE("info: stream count %d\n", format_context->nb_streams);
	audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, const_cast<const AVCodec**>(&codec), 0);
	if (audio_stream_index < 0) {
		NATIVE_TRACE("err: no audio stream found\n");
		return fail_load();
	}

	AVStream* current_stream = format_context->streams[audio_stream_index];
	codec = const_cast<AVCodec*>(avcodec_find_decoder(current_stream->codecpar->codec_id));
	if (!codec)
	{
		NATIVE_TRACE("warn: no valid decoder found, stream id = %d!\n", audio_stream_index);
		return fail_load();
	}

	NATIVE_TRACE("info: open stream id %d, format=%d, sample_rate=%d, channels=%d, channel_layout=%d\n",
		audio_stream_index,
		current_stream->codecpar->format,
		current_stream->codecpar->sample_rate,
		current_stream->codecpar->ch_layout.nb_channels,
		current_stream->codecpar->ch_layout.order);

	int image_stream_id = -1;
	std::uint64_t attached_picture_bytes = 0;

	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		if (AVStream* stream = format_context->streams[i]; stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			if (image_stream_id < 0)
			{
				NATIVE_TRACE("info: open stream id %d read attaching pic\n", i);
				image_stream_id = static_cast<int>(i);
			}
			if (stream->attached_pic.size > 0)
			{
				attached_picture_bytes +=
					static_cast<std::uint64_t>(stream->attached_pic.size);
			}
		}
	}
	const std::uint64_t estimated_audio_stream_length_bytes =
		static_cast<std::uint64_t>(file_len) > attached_picture_bytes
			? static_cast<std::uint64_t>(file_len) - attached_picture_bytes
			: 0;

	if (image_stream_id != -1 && !skip_album_art_loading && this->file_extension != L"ncm")
	{
		publish_message({PlayerMessageType::AlbumArtInitialized,
			get_id3_album_art_stream(image_stream_id)});
	}
	read_metadata();

	// 从0ms开始读取
	avformat_seek_file(format_context, -1, INT64_MIN, 0, INT64_MAX, 0);
	// codec is not null
	// 建立解码器上下文
	codec_context = avcodec_alloc_context3(codec);
	if (codec_context == nullptr)
	{
		NATIVE_TRACE("err: avcodec_alloc_context3 failed\n");
		return fail_load();
	}
	avcodec_parameters_to_context(codec_context, format_context->streams[audio_stream_index]->codecpar);

	// 降低错误容忍度
	codec_context->err_recognition = AV_EF_IGNORE_ERR | AV_EF_COMPLIANT;
	// 错误隐藏
	codec_context->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
	// 跳过坏帧
	codec_context->skip_frame = AVDISCARD_NONREF;

	// 解码文件
	res = avcodec_open2(codec_context, codec, nullptr);
	if (res)
	{
		const auto reason = GetFFmpegErrorMessage(res);
		NATIVE_TRACE("err: avcodec_open2 failed, reason = %s\n", reason.c_str());
		return fail_load();
	}

	const AVCodecParameters& codec_parameters =
		*format_context->streams[audio_stream_index]->codecpar;
	const int source_sample_rate = codec_context->sample_rate > 0
		? codec_context->sample_rate
		: codec_parameters.sample_rate;
	const int source_channel_count = codec_context->ch_layout.nb_channels > 0
		? codec_context->ch_layout.nb_channels
		: codec_parameters.ch_layout.nb_channels;
	const AVChannelLayout& source_channel_layout =
		codec_context->ch_layout.nb_channels > 0
			? codec_context->ch_layout
			: codec_parameters.ch_layout;
	const std::uint64_t source_channel_mask =
		GetNativeChannelMask(source_channel_layout);
	const int source_bit_depth =
		GetEffectiveSourceBitDepth(*codec_context, codec_parameters);
	audio_source_format_info = {
		GetAudioChannelTypeId(source_channel_count, source_channel_mask),
		source_sample_rate,
		source_bit_depth > 0
			? source_bit_depth
			: static_cast<int>(AudioBitDepth::Unknown)
	};
	NATIVE_TRACE(
		"info: audio source format: channel_type_id=%d, sample_rate=%d, bit_depth=%d\n",
		audio_source_format_info.channel_type_id,
		audio_source_format_info.sample_rate,
		audio_source_format_info.bit_depth);
	update_audio_quality_flags(
		estimated_audio_stream_length_bytes, source_sample_rate);

	const DecodedAudioFormat decoded_input_format = MakeDecodedAudioFormat(
		source_sample_rate,
		codec_context->sample_fmt,
		source_channel_layout,
		audio_source_format_info.bit_depth);
	const bool bit_perfect_handshake = audio_sink_ &&
		audio_sink_->HandshakeInputFormat(decoded_input_format);
	is_audio_file_and_sink_bit_perfect.store(
		bit_perfect_handshake, std::memory_order_release);
	NATIVE_TRACE(
		"info: source/sink exact-format handshake: %s\n",
		bit_perfect_handshake ? "accepted" : "normalization required");

	// avoid ffmpeg warning
	codec_context->pkt_timebase = format_context->streams[audio_stream_index]->time_base;
	// set parallel decode (flac, wav..
	av_opt_set_int(codec_context, "threads", 0, 0);

	// Initialize the FIFO with the normalized format submitted to the sink.
	fifo_audio_channels = audio_output_format.channel_count;
	fifo_audio_sample_fmt = audio_output_format.sample_format;
	reset_audio_pipeline_buffering();
	if (!audio_fifo) {
		res = initialize_audio_fifo(fifo_audio_sample_fmt,
			fifo_audio_channels,
			audio_fifo_high_watermark_samples.load() + sink_submit_frame_size);
		if (res < 0) {
			NATIVE_TRACE("err: initialize_audio_fifo failed\n");
			return fail_load();
		}
	}

	// init decoder
	frame.reset(av_frame_alloc());
	normalized_frame.reset(
		bit_perfect_handshake ? nullptr : av_frame_alloc());
	packet = av_packet_alloc();
	if (!frame || (!bit_perfect_handshake && !normalized_frame) || !packet)
	{
		NATIVE_TRACE("err: allocate decode frame or packet failed\n");
		return fail_load();
	}

	reset_audio_normalization_filter();
	if (!bit_perfect_handshake && init_audio_normalization_filter() < 0)
	{
		release_audio_session_resources();
		return -1;
	}

	if (!audio_sink_)
		return fail_load();
	const auto generation = audio_sink_->BeginStream();
	publish_observer_generation(generation);
	init_decoder_thread();
	return 0;
}

void MusicPlayerLibrary::AudioFile::release_audio_context()
{
	audio_source_format_info = {};
	is_audio_file_and_sink_bit_perfect.store(false, std::memory_order_release);
	reset_audio_source_bitrate();
	reset_audio_quality_flags();
	length.store(0.0, std::memory_order_relaxed);
	if (avio_context)
	{
		avio_context_free(&avio_context);
		avio_context = nullptr;
		buffer = nullptr;
	}
	else if (buffer)
	{
		av_freep(&buffer);
		buffer = nullptr;
	}
	if (format_context)
	{
		// 释放文件解析上下文
		avformat_close_input(&format_context);
		format_context = nullptr;
	}

	if (codec_context)
	{
		// 释放解码器上下文
		avcodec_free_context(&codec_context);
		codec_context = nullptr;
	}
	uninitialize_audio_fifo();
	if (file_stream)
	{
		file_stream.reset();
	}
}

void MusicPlayerLibrary::AudioFile::reset_audio_quality_flags() noexcept
{
	average_audio_bitrate_bits_per_second.store(0.0, std::memory_order_relaxed);
	is_loseless_audio.store(false, std::memory_order_relaxed);
	is_hi_res_audio.store(false, std::memory_order_relaxed);
}

void MusicPlayerLibrary::AudioFile::update_audio_quality_flags(
	const std::uint64_t stream_length_bytes,
	const int source_sample_rate) noexcept
{
	const double duration_seconds =
		GetAudioStreamDurationSeconds(format_context, audio_stream_index);
	const double average_bitrate = CalculateAverageAudioBitrateBitsPerSecond(
		stream_length_bytes, duration_seconds);

	length.store(duration_seconds, std::memory_order_relaxed);
	average_audio_bitrate_bits_per_second.store(
		average_bitrate, std::memory_order_relaxed);
	is_loseless_audio.store(
		MusicPlayerLibrary::IsLoselessAudio(average_bitrate),
		std::memory_order_relaxed);
	is_hi_res_audio.store(
		MusicPlayerLibrary::IsHiResAudio(source_sample_rate),
		std::memory_order_relaxed);
	NATIVE_TRACE(
		"info: audio quality estimate: average_bitrate=%.0f bps, is_loseless_audio=%d, is_hi_res_audio=%d\n",
		average_bitrate,
		is_loseless_audio.load(std::memory_order_relaxed) ? 1 : 0,
		is_hi_res_audio.load(std::memory_order_relaxed) ? 1 : 0);
}

void MusicPlayerLibrary::AudioFile::reset_audio_context()
{
	// release_audio_context();
	file_stream_end = false;
	normalized_stream_end = false;
	// A new generation must reopen the source and normalizer worker loops.
	user_request_stop.store(false);
	if (is_audio_context_initialized()) {
		stop_audio_decode();
		if (av_seek_frame(format_context, audio_stream_index, 0, AVSEEK_FLAG_BACKWARD) >= 0)
			reset_audio_source_bitrate();
		avcodec_flush_buffers(codec_context);
		// 重置滤镜图；精确匹配的源/输出格式继续走字节直通路径
		reset_audio_normalization_filter();
		if (!is_audio_file_and_sink_bit_perfect.load(std::memory_order_acquire))
		{
			NATIVE_TRACE("info: audio context reset, rebuilding filter graph\n");
			if (init_audio_normalization_filter() < 0)
			{
				playback_state.store(audio_playback_state_stopped);
				return;
			}
		}
	}
	playback_state.store(audio_playback_state_init);
	reset_audio_fifo();
	if (audio_sink_)
	{
		const auto generation = audio_sink_->BeginStream();
		publish_observer_generation(generation);
	}
	init_decoder_thread();
	// load_audio_context_from_file_stream();
}

int MusicPlayerLibrary::AudioFile::get_average_audio_bitrate() const
{
	return average_audio_bitrate_bits_per_second.load();
}

bool MusicPlayerLibrary::AudioFile::is_audio_context_initialized()
{
	return avio_context
		&& format_context
		&& codec_context
		&& file_stream;
}

std::vector<std::uint8_t> MusicPlayerLibrary::AudioFile::get_id3_album_art_stream(const int stream_index)
{
	if (!format_context) return {};

	// stream_index = attached pic
	const AVPacket& pkt = format_context->streams[stream_index]->attached_pic;
	if (pkt.data == nullptr || pkt.size <= 0)
		return {};
	return {pkt.data, pkt.data + static_cast<std::size_t>(pkt.size)};
}

void MusicPlayerLibrary::AudioFile::require_download_ncm_album_art(const std::wstring& url)
{
	publish_message({PlayerMessageType::NcmAlbumArtDownloadRequired, url});
}

void MusicPlayerLibrary::AudioFile::publish_message(PlayerMessage message) const
{
	if (message.type == PlayerMessageType::TimeChanged && suppress_time_events.load())
		return;
	if (message_sink_)
		message_sink_->Publish(message);
}

void MusicPlayerLibrary::AudioFile::read_metadata()
{
	auto convert_utf8 = [](const char* utf_8_str) {
		return LocaleConverter::GetUtf16StringFromUtf8String(
			utf_8_str == nullptr ? std::string() : std::string(utf_8_str));
		};
	auto read_metadata_iter = [&](const AVDictionaryEntry* tag) {
		std::wstring key = convert_utf8(tag->key);
		std::wstring value = convert_utf8(tag->value);
		if (EqualsAsciiIgnoreCase<wchar_t>(key, L"title") && song_title.empty()) {
			song_title = value;
			NATIVE_TRACE(L"info: song title: %s\n", song_title.c_str());
		}
		else if (EqualsAsciiIgnoreCase<wchar_t>(key, L"artist") && song_artist.empty()) {
			song_artist = value;
			NATIVE_TRACE(L"info: song artist: %s\n", song_artist.c_str());
		}
		else
		{
			auto lower = key;
			std::ranges::transform(
				lower,
				lower.begin(),
				[](const wchar_t value)
				{
					return ToLowerAscii(value);
				});
			if (lower.find(L"lyric") != std::wstring::npos)
			{
				this->id3_string_lyric = value;
				NATIVE_TRACE("info: song contains lyric in metadata\n");
			}
			else
			{
				NATIVE_TRACE(L"info: key %s = %s\n", key.c_str(), value.c_str());
			}
		}
	};

	const AVDictionaryEntry* tag = nullptr;
	while ((tag = av_dict_get(format_context->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
		read_metadata_iter(tag);
	}

	// decode album title & artist
	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		AVStream* stream = format_context->streams[i];
		tag = nullptr;
		while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
			read_metadata_iter(tag);
		}
	}
}

// playback area
bool MusicPlayerLibrary::AudioFile::wait_frame_ready(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(frame_event_mutex);
	if (!frame_ready_cv.wait_for(lock, timeout, [this]
		{
			return frame_ready_requested || playback_state.load() == audio_playback_state_stopped;
		}))
	{
		return false;
	}
	frame_ready_requested = false;
	return true;
}

bool MusicPlayerLibrary::AudioFile::wait_frame_underrun(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(frame_event_mutex);
	if (!frame_underrun_cv.wait_for(lock, timeout, [this]
		{
			return frame_underrun_requested || playback_state.load() == audio_playback_state_stopped;
		}))
	{
		return false;
	}
	frame_underrun_requested = false;
	return true;
}

void MusicPlayerLibrary::AudioFile::notify_frame_ready()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_ready_requested = true;
	}
	frame_ready_cv.notify_one();
}

void MusicPlayerLibrary::AudioFile::notify_frame_underrun()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_underrun_requested = true;
	}
	frame_underrun_cv.notify_one();
}

void MusicPlayerLibrary::AudioFile::reset_frame_notifications()
{
	std::lock_guard lock(frame_event_mutex);
	frame_ready_requested = false;
	frame_underrun_requested = false;
}

void MusicPlayerLibrary::AudioFile::notify_all_frame_notifications()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_ready_requested = true;
		frame_underrun_requested = true;
	}
	frame_ready_cv.notify_all();
	frame_underrun_cv.notify_all();
	decoded_frame_queue_cv.notify_all();
}

int MusicPlayerLibrary::AudioFile::get_audio_fifo_low_watermark()
{
	return std::max(
		sink_submit_frame_size,
		audio_fifo_high_watermark_samples.load() / 4);
}

int MusicPlayerLibrary::AudioFile::get_audio_fifo_high_watermark()
{
	return audio_fifo_high_watermark_samples.load();
}

bool MusicPlayerLibrary::AudioFile::wait_for_audio_fifo_room()
{
	while (playback_state.load() != audio_playback_state_stopped &&
		!user_request_stop.load())
	{
		{
			std::lock_guard fifo_lock(audio_fifo_mutex);
			if (get_audio_fifo_cached_samples_size() <
				get_audio_fifo_high_watermark())
			{
				return true;
			}
		}
		wait_frame_underrun(AudioWorkerPollInterval);
	}
	return false;
}

int MusicPlayerLibrary::AudioFile::get_decoded_frame_queue_high_watermark()
{
	return decoded_frame_queue_high_watermark_samples.load();
}

void MusicPlayerLibrary::AudioFile::reset_audio_pipeline_buffering()
{
	std::lock_guard buffering_lock(audio_pipeline_buffering_mutex);
	const int fifo_target = std::max(
		SamplesForMilliseconds(audio_output_format.sample_rate, initial_buffering_profile.fifo_target_milliseconds),
		sink_submit_frame_size * 8);
	const int decoded_queue_target = std::max(
		SamplesForMilliseconds(audio_output_format.sample_rate, initial_buffering_profile.decoded_queue_target_milliseconds),
		sink_submit_frame_size * 2);

	audio_fifo_high_watermark_samples.store(fifo_target);
	decoded_frame_queue_high_watermark_samples.store(decoded_queue_target);
	decoder_slow_operation_count.store(0);
	normalizer_slow_operation_count.store(0);
	const auto allow_immediate_growth_at =
		std::chrono::steady_clock::now() - PipelineBufferGrowthCooldown;
	last_pipeline_buffer_growth_ns.store(
		SteadyClockNanoseconds(allow_immediate_growth_at));

	NATIVE_TRACE(
		"info: audio pipeline initial buffering: fft=%.2f us, fifo=%d samples (%d ms), decoded queue=%d samples (%d ms)\n",
		initial_buffering_profile.small_fft_microseconds,
		fifo_target,
		initial_buffering_profile.fifo_target_milliseconds,
		decoded_queue_target,
		initial_buffering_profile.decoded_queue_target_milliseconds);
}

void MusicPlayerLibrary::AudioFile::observe_audio_pipeline_stage_duration(
	const audio_pipeline_stage stage,
	const double elapsed_milliseconds,
	const int processed_samples,
	const int processed_sample_rate)
{
	const int valid_sample_rate = processed_sample_rate > 0
		? processed_sample_rate
		: (audio_output_format.sample_rate > 0
			? audio_output_format.sample_rate
			: StandardAudioSampleRate);
	const double audio_duration_milliseconds = processed_samples > 0
		? static_cast<double>(processed_samples) * 1000.0 / valid_sample_rate
		: static_cast<double>(sink_submit_frame_size) * 1000.0 / valid_sample_rate;
	// 解码延迟判断标准：解码耗时>=8.0ms；或解码耗时>=该时段产出样本时长*0.75
	const double slow_threshold_milliseconds = std::max(
		8.0,
		audio_duration_milliseconds * 0.75);

	std::atomic_uint& slow_operation_count = stage == audio_pipeline_stage::decoder
		? decoder_slow_operation_count
		: normalizer_slow_operation_count;
	if (elapsed_milliseconds <= slow_threshold_milliseconds)
	{
		slow_operation_count.store(0);
		return;
	}

	const unsigned int consecutive_slow_operations =
		slow_operation_count.fetch_add(1) + 1;
	const bool severely_slow = elapsed_milliseconds >=
		std::max(slow_threshold_milliseconds * 2.0, 30.0);
	if (!severely_slow && consecutive_slow_operations < 2)
		return;

	slow_operation_count.store(0);
	grow_audio_pipeline_buffers(
		stage == audio_pipeline_stage::decoder ? "decoder" : "normalizer",
		elapsed_milliseconds,
		slow_threshold_milliseconds);
}

void MusicPlayerLibrary::AudioFile::observe_and_reset_decoder_pending_work(
	const int processed_samples,
	const int processed_sample_rate)
{
	const int effective_sample_rate = processed_sample_rate > 0
		? processed_sample_rate
		: (codec_context
			? codec_context->sample_rate
			: audio_output_format.sample_rate);
	observe_audio_pipeline_stage_duration(
		audio_pipeline_stage::decoder,
		decoder_pending_work_milliseconds,
		processed_samples,
		effective_sample_rate);
	decoder_pending_work_milliseconds = 0.0;
}

void MusicPlayerLibrary::AudioFile::grow_audio_pipeline_buffers(
	const char* slow_stage,
	const double elapsed_milliseconds,
	const double slow_threshold_milliseconds)
{
	std::lock_guard buffering_lock(audio_pipeline_buffering_mutex);
	const std::int64_t now_ns = SteadyClockNowNanoseconds();
	const std::int64_t cooldown_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		PipelineBufferGrowthCooldown).count();
	if (now_ns - last_pipeline_buffer_growth_ns.load() < cooldown_ns)
		return;

	const int valid_sample_rate = audio_output_format.sample_rate > 0
		? audio_output_format.sample_rate
		: StandardAudioSampleRate;
	const int old_fifo_target = audio_fifo_high_watermark_samples.load();
	const int old_queue_target = decoded_frame_queue_high_watermark_samples.load();
	const int max_fifo_target = SamplesForMilliseconds(valid_sample_rate, 750);
	const int max_queue_target = SamplesForMilliseconds(valid_sample_rate, 200);
	const int new_fifo_target = std::min(
		max_fifo_target,
		std::max(
			old_fifo_target + old_fifo_target / 2,
			old_fifo_target + std::max(
				SamplesForMilliseconds(valid_sample_rate, 50),
				sink_submit_frame_size * 4)));
	const int new_queue_target = std::min(
		max_queue_target,
		std::max(
			old_queue_target + old_queue_target / 2,
			old_queue_target + std::max(
				SamplesForMilliseconds(valid_sample_rate, 10),
				sink_submit_frame_size)));
	if (new_fifo_target <= old_fifo_target && new_queue_target <= old_queue_target)
		return;

	if (audio_fifo && new_fifo_target > old_fifo_target)
	{
		std::lock_guard fifo_lock(audio_fifo_mutex);
		const int current_fifo_size = get_audio_fifo_cached_samples_size();
		resize_audio_fifo(std::max(
			new_fifo_target + sink_submit_frame_size,
			current_fifo_size + sink_submit_frame_size));
	}

	audio_fifo_high_watermark_samples.store(new_fifo_target);
	decoded_frame_queue_high_watermark_samples.store(new_queue_target);
	last_pipeline_buffer_growth_ns.store(now_ns);
	decoded_frame_queue_cv.notify_all();
	notify_frame_underrun();
	NATIVE_TRACE(
		"warn: %s stage slow (%.2f ms, threshold %.2f ms); grow fifo %d->%d samples and decoded queue %d->%d samples\n",
		slow_stage,
		elapsed_milliseconds,
		slow_threshold_milliseconds,
		old_fifo_target,
		new_fifo_target,
		old_queue_target,
		new_queue_target);
}

bool MusicPlayerLibrary::AudioFile::is_audio_pipeline_running()
{
	return decoder_is_running.load() || normalizer_is_running.load();
}

bool MusicPlayerLibrary::AudioFile::queue_decoded_frame(AVFrame* decoded_frame)
{
	if (!decoded_frame)
		return false;

	UniqueAvFrame queued_frame(av_frame_alloc());
	if (!queued_frame)
	{
		FFMPEG_CRITICAL_ERROR(AVERROR(ENOMEM));
		return false;
	}
	av_frame_move_ref(queued_frame.get(), decoded_frame);
	const int frame_samples = queued_frame->nb_samples;

	std::unique_lock lock(decoded_frame_queue_mutex);
	while (!decoded_frame_queue_abort
		&& playback_state.load() != audio_playback_state_stopped
		&& decoded_frame_queue_samples >= get_decoded_frame_queue_high_watermark())
	{
		decoded_frame_queue_cv.wait_for(lock, AudioWorkerPollInterval);
	}

	if (decoded_frame_queue_abort || playback_state.load() == audio_playback_state_stopped)
	{
		return false;
	}

	decoded_frame_queue.push_back(std::move(queued_frame));
	decoded_frame_queue_samples += frame_samples;
	lock.unlock();
	decoded_frame_queue_cv.notify_all();
	return true;
}

MusicPlayerLibrary::UniqueAvFrame
MusicPlayerLibrary::AudioFile::pop_decoded_frame()
{
	std::unique_lock lock(decoded_frame_queue_mutex);
	decoded_frame_queue_cv.wait(lock, [this]
		{
			return decoded_frame_queue_abort
				|| playback_state.load() == audio_playback_state_stopped
				|| !decoded_frame_queue.empty()
				|| decoded_frame_queue_eof;
		});

	if (decoded_frame_queue_abort || playback_state.load() == audio_playback_state_stopped)
		return {};
	if (decoded_frame_queue.empty())
		return {};

	UniqueAvFrame frame = std::move(decoded_frame_queue.front());
	decoded_frame_queue.pop_front();
	decoded_frame_queue_samples -= frame ? frame->nb_samples : 0;
	if (decoded_frame_queue_samples < 0)
		decoded_frame_queue_samples = 0;
	lock.unlock();
	decoded_frame_queue_cv.notify_all();
	return frame;
}

void MusicPlayerLibrary::AudioFile::signal_decoded_frame_queue_eof()
{
	{
		std::lock_guard lock(decoded_frame_queue_mutex);
		decoded_frame_queue_eof = true;
	}
	decoded_frame_queue_cv.notify_all();
}

void MusicPlayerLibrary::AudioFile::release_audio_session_resources()
{
	close_audio_session();
	reset_audio_normalization_filter();
	release_audio_context();
}

void MusicPlayerLibrary::AudioFile::abort_decoded_frame_queue_waiters()
{
	{
		std::lock_guard lock(decoded_frame_queue_mutex);
		decoded_frame_queue_abort = true;
	}
	decoded_frame_queue_cv.notify_all();
	notify_all_frame_notifications();
}

void MusicPlayerLibrary::AudioFile::reset_decoded_frame_queue(bool abort_waiters)
{
	std::lock_guard lock(decoded_frame_queue_mutex);
	decoded_frame_queue.clear();
	decoded_frame_queue_samples = 0;
	decoded_frame_queue_eof = false;
	decoded_frame_queue_abort = abort_waiters;
	decoded_frame_queue_cv.notify_all();
}

inline int MusicPlayerLibrary::AudioFile::initialize_audio_session()
{
	if (!codec_context || !audio_sink_ || !audio_sink_->IsInitialized())
		return -1;
	audio_output_format = audio_sink_->GetOutputFormat();
	playback_state.store(audio_playback_state_init);
	return 0;
}

std::vector<std::shared_ptr<MusicPlayerLibrary::IAudioObserve>>
MusicPlayerLibrary::AudioFile::snapshot_audio_observers() const
{
	std::lock_guard lock(audio_observers_mutex_);
	return audio_observers_;
}

void MusicPlayerLibrary::AudioFile::publish_observer_generation(
	const AudioStreamGeneration generation)
{
	std::lock_guard dispatch_lock(audio_observer_dispatch_mutex_);
	// Publish the generation under the same serialization boundary used by
	// Subscribe(). A late subscriber therefore either joins before this batch
	// or initializes itself afterward, but can never receive both paths.
	normalized_stream_frames_.store(0, std::memory_order_release);
	stream_origin_seconds_.store(pts_seconds.load(), std::memory_order_release);
	audio_stream_generation_.store(generation);
	for (const auto& observe : snapshot_audio_observers())
	{
		if (!observe)
			continue;
		observe->OnFormat(audio_output_format, generation);
		observe->OnReset(generation);
	}
}

void MusicPlayerLibrary::AudioFile::notify_observers_reset(
	const AudioStreamGeneration generation)
{
	std::lock_guard dispatch_lock(audio_observer_dispatch_mutex_);
	for (const auto& observe : snapshot_audio_observers())
		if (observe)
			observe->OnReset(generation);
}

void MusicPlayerLibrary::AudioFile::notify_observers_pcm(
	const NormalizedPcmBlock& block)
{
	std::lock_guard dispatch_lock(audio_observer_dispatch_mutex_);
	for (const auto& observe : snapshot_audio_observers())
		if (observe)
			observe->OnPcm(block);
}

void MusicPlayerLibrary::AudioFile::notify_observers_pcm_frame(
	const AVFrame& frame)
{
	const auto frame_count = static_cast<std::uint32_t>(frame.nb_samples);
	const auto byte_count = static_cast<std::size_t>(frame_count) *
		audio_output_format.wave_format.Format.nBlockAlign;
	const auto stream_frame_offset = normalized_stream_frames_.fetch_add(
		frame_count, std::memory_order_acq_rel);
	const double stream_origin = stream_origin_seconds_.load(
		std::memory_order_acquire);
	const NormalizedPcmBlock observed_block{
		.bytes = std::span<const std::uint8_t>(
			frame.extended_data[0], byte_count),
		.frame_count = frame_count,
		.pts_seconds = stream_origin +
			static_cast<double>(stream_frame_offset) /
				audio_output_format.sample_rate,
		.stream_frame_offset = stream_frame_offset,
		.generation = audio_stream_generation_.load(),
		.end_of_stream = false
	};
	notify_observers_pcm(observed_block);
}

void MusicPlayerLibrary::AudioFile::notify_observers_end_of_stream(
	const AudioStreamGeneration generation)
{
	std::lock_guard dispatch_lock(audio_observer_dispatch_mutex_);
	for (const auto& observe : snapshot_audio_observers())
		if (observe)
			observe->OnEndOfStream(generation);
}

inline void MusicPlayerLibrary::AudioFile::close_audio_session()
{
	user_request_stop.store(true);
	// Stop source workers while retaining the connected sink and output device.
	if (audio_player_worker_thread.joinable())
	{
		playback_state.store(audio_playback_state_stopped);
		audio_player_worker_thread.request_stop();
		notify_all_frame_notifications();
		audio_player_worker_thread.join();
	}
	stop_audio_decode();
	reset_decoded_frame_queue(true);
	if (audio_sink_)
		audio_sink_->AbortStream();
	frame.reset();
	normalized_frame.reset();
	if (packet) {
		av_packet_free(&packet);
		packet = nullptr;
	}
	notify_observers_reset(audio_stream_generation_.load());
}

void MusicPlayerLibrary::AudioFile::start_audio_output_sink()
{
	if (!audio_sink_ || !audio_sink_->Start())
		throw std::runtime_error("failed to start the audio output sink");
	playback_state.store(audio_playback_state_playing);
	publish_message({PlayerMessageType::Started});
}

double MusicPlayerLibrary::AudioFile::update_elapsed_time(
	const AudioSinkState& state) noexcept
{
	const auto sample_rate = audio_output_format.wave_format.Format.nSamplesPerSec;
	double current = pts_seconds.load();
	if (sample_rate > 0)
	{
		current += static_cast<double>(state.samples_played) / sample_rate;
	}
	if (const double duration = length.load(); duration > 0.0)
		current = std::min(current, duration);
	elapsed_time.store(current);
	return current;
}

void MusicPlayerLibrary::AudioFile::audio_playback_worker_thread()
{
	const FAudioWaveFormatEx& wfx = audio_output_format.wave_format.Format;
	AudioSinkState state{};
	bool eos_submitted = false;
	std::uint64_t submitted_stream_frames = 0;
	const bool use_frame_based_backpressure =
		audio_sink_ && audio_sink_->IsExclusiveModeEnabled();
	const auto queue_high_watermark_frames = (std::max)(
		static_cast<std::uint64_t>(sink_submit_frame_size) * 2,
		static_cast<std::uint64_t>(wfx.nSamplesPerSec) / 10);
	const auto queue_low_watermark_frames = (std::max)(
		static_cast<std::uint64_t>(sink_submit_frame_size),
		queue_high_watermark_frames / 2);
	const auto start_prefill_frames = use_frame_based_backpressure
		? queue_high_watermark_frames
		: static_cast<std::uint64_t>(sink_submit_frame_size);

	while (true)
	{
		if (!wait_frame_ready(PlaybackPollInterval))
		{
			const int cached_sample_size = get_audio_fifo_cached_samples_size();
			if (playback_state.load() == audio_playback_state_stopped)
				break;
			if (playback_state.load() == audio_playback_state_init ||
				playback_state.load() == audio_playback_state_decoder_exit_pre_stop ||
				cached_sample_size > get_audio_fifo_low_watermark())
			{
				if (cached_sample_size < get_audio_fifo_low_watermark())
					notify_frame_underrun();
			}
			else if (normalized_stream_end.load())
			{
				notify_frame_ready();
			}
			else
			{
				notify_frame_underrun();
				continue;
			}
		}

		std::lock_guard playback_lock(audio_playback_mutex);
		int fifo_size = get_audio_fifo_cached_samples_size();
		if (fifo_size < 0 && is_audio_pipeline_running())
		{
			std::this_thread::sleep_for(PlaybackPollInterval);
			continue;
		}

		state = audio_sink_ ? audio_sink_->GetState() : AudioSinkState{};
		if (state.error_code != 0)
			throw std::runtime_error("audio output sink reported a voice error");
		if (user_request_stop.load())
		{
			NATIVE_TRACE("info: user requested audio source stop\n");
			break;
		}

		if (playback_state.load() == audio_playback_state_decoder_exit_pre_stop)
		{
			if (!state.stream_ended)
			{
				if (audio_sink_)
					(void)audio_sink_->WaitForStreamEnd(PlaybackPollInterval);
				state = audio_sink_ ? audio_sink_->GetState() : AudioSinkState{};
				if (state.error_code != 0)
					throw std::runtime_error("audio output sink failed while draining EOS");
				if (state.samples_played > 0)
				{
					publish_message({
						PlayerMessageType::TimeChanged,
						update_elapsed_time(state)
					});
				}
				continue;
			}

			NATIVE_TRACE("info: output sink reported end of stream\n");
			if (audio_sink_)
				audio_sink_->Stop();
			publish_message({PlayerMessageType::Stopped});
			pts_seconds = 0.0;
			playback_state.store(audio_playback_state_stopped);
			break;
		}

		if (!is_audio_pipeline_running() && fifo_size == 0)
		{
			if (!eos_submitted)
			{
				// An empty stream has no real PCM buffer on which to place EOS, so
				// the sink must synthesize a marker and the route is not bit-perfect.
				if (submitted_stream_frames == 0)
				{
					is_audio_file_and_sink_bit_perfect.store(
						false, std::memory_order_release);
				}
				if (!audio_sink_ || !audio_sink_->EndStream())
					throw std::runtime_error("failed to submit the audio sink EOS marker");
				eos_submitted = true;
				if (playback_state.load() == audio_playback_state_init)
				{
					start_audio_output_sink();
				}
			}
			NATIVE_TRACE("info: normalized source drained; waiting for sink EOS\n");
			playback_state.store(audio_playback_state_decoder_exit_pre_stop);
			continue;
		}

		AvSamplesBuffer fifo_buffer;
		int read_samples = 0;
		bool is_final_block = false;
		{
			std::lock_guard fifo_lock(audio_fifo_mutex);
			fifo_size = get_audio_fifo_cached_samples_size();
			if (fifo_size <= 0)
			{
				notify_frame_underrun();
				continue;
			}
			if (is_audio_pipeline_running() && !normalized_stream_end.load() &&
				fifo_size < sink_submit_frame_size)
			{
				notify_frame_underrun();
				continue;
			}

			const int fifo_read_size = std::min(sink_submit_frame_size, fifo_size);
			if (const int allocation_result = fifo_buffer.Allocate(
				fifo_audio_channels, fifo_read_size, fifo_audio_sample_fmt);
				allocation_result < 0)
			{
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(allocation_result);
				break;
			}
			read_samples = read_samples_from_fifo(fifo_buffer.Get(), fifo_read_size);
			if (read_samples < 0)
			{
				NATIVE_TRACE("err: read normalized samples from FIFO failed, code=%d\n",
					read_samples);
				if (user_request_stop.load())
					break;
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(read_samples);
				break;
			}
			is_final_block = normalized_stream_end.load() &&
				get_audio_fifo_cached_samples_size() == 0;
		}

		if (read_samples == 0)
		{
			std::this_thread::sleep_for(PlaybackPollInterval);
			continue;
		}

		while ((use_frame_based_backpressure
			? state.queued_frames >= queue_high_watermark_frames
			: state.buffers_queued >= 32) &&
			!user_request_stop.load())
		{
			std::this_thread::sleep_for(PlaybackPollInterval);
			state = audio_sink_ ? audio_sink_->GetState() : AudioSinkState{};
			if (state.error_code != 0)
				throw std::runtime_error(
					"audio output sink failed while waiting for buffer capacity");
		}
		if (user_request_stop.load())
			break;

		const auto audio_byte_count = static_cast<std::uint64_t>(read_samples) *
			wfx.nBlockAlign;
		if (audio_byte_count >
			(std::numeric_limits<std::uint32_t>::max)())
		{
			throw std::overflow_error("normalized PCM block is too large");
		}
		const auto audio_bytes = static_cast<std::uint32_t>(audio_byte_count);
		const double stream_origin = stream_origin_seconds_.load(
			std::memory_order_acquire);
		const NormalizedPcmBlock pcm_block{
			.bytes = std::span<const std::uint8_t>(
				fifo_buffer.FirstPlane(), audio_bytes),
			.frame_count = static_cast<std::uint32_t>(read_samples),
			.pts_seconds = stream_origin +
				static_cast<double>(submitted_stream_frames) / wfx.nSamplesPerSec,
			.stream_frame_offset = submitted_stream_frames,
			.generation = audio_stream_generation_.load(),
			.end_of_stream = is_final_block
		};
		const bool submitted = audio_sink_ && audio_sink_->Submit(pcm_block);
		fifo_buffer.Reset();
		if (!submitted)
		{
			NATIVE_TRACE("err: submit normalized PCM to audio sink failed\n");
			playback_state.store(audio_playback_state_stopped);
			break;
		}
		submitted_stream_frames += static_cast<std::uint64_t>(read_samples);
		if (is_final_block)
		{
			eos_submitted = true;
		}

		const bool prefill_ready = submitted_stream_frames >=
			start_prefill_frames || is_final_block;
		if (playback_state.load() == audio_playback_state_init && prefill_ready)
		{
			start_audio_output_sink();
			if (!use_frame_based_backpressure)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		state = audio_sink_->GetState();
		const double current_elapsed_time = update_elapsed_time(state);
		const double submitted_time_ms =
			static_cast<double>(read_samples) * 1000.0 / wfx.nSamplesPerSec;
		if (message_interval_timer > message_interval ||
			message_interval_timer < 0.0f)
		{
			message_interval_timer = 0.0f;
			publish_message({PlayerMessageType::TimeChanged, current_elapsed_time});
		}
		else
		{
			message_interval_timer += static_cast<float>(submitted_time_ms);
		}

		std::lock_guard fifo_event_lock(audio_fifo_mutex);
		if (get_audio_fifo_cached_samples_size() < get_audio_fifo_low_watermark())
		{
			notify_frame_underrun();
		}
		else if (use_frame_based_backpressure
			? state.queued_frames < queue_low_watermark_frames
			: state.buffers_queued < 16)
		{
			notify_frame_ready();
		}
	}
}

void MusicPlayerLibrary::AudioFile::reset_audio_source_bitrate() noexcept
{
	audio_source_bitrate_tracker.Reset();
}

void MusicPlayerLibrary::AudioFile::observe_audio_source_packet(
	const AVPacket& source_packet) noexcept
{
	if (source_packet.size > 0)
		audio_source_bitrate_tracker.ObserveEncodedBytes(
			static_cast<std::uint64_t>(source_packet.size));
}

void MusicPlayerLibrary::AudioFile::observe_audio_source_frame(
	const AVFrame& decoded_frame) noexcept
{
	const int sample_rate = decoded_frame.sample_rate > 0
		? decoded_frame.sample_rate
		: (codec_context ? codec_context->sample_rate : 0);
	if (decoded_frame.nb_samples <= 0 || sample_rate <= 0)
		return;

	audio_source_bitrate_tracker.ObserveDecodedSamples(
		decoded_frame.nb_samples, sample_rate);
}

void MusicPlayerLibrary::AudioFile::audio_decode_worker_thread()
{
	bool is_eof = false;
	bool decoder_flushed = false;
	decoder_pending_work_milliseconds = 0.0;
	const auto timed_decoder_call = [this](auto&& operation)
	{
		const auto begin = std::chrono::steady_clock::now();
		const int result = operation();
		decoder_pending_work_milliseconds += ElapsedMilliseconds(
			std::chrono::steady_clock::now() - begin);
		return result;
	};
	while (true) {
		if (playback_state.load() == audio_playback_state_stopped) {
			NATIVE_TRACE("info: playback stopped, decoder thread exiting\n");
			break;
		}

		{
			std::unique_lock queue_lock(decoded_frame_queue_mutex);
			if (decoded_frame_queue_samples >= get_decoded_frame_queue_high_watermark())
			{
				decoded_frame_queue_cv.wait_for(queue_lock, AudioWorkerPollInterval, [this]
					{
						return decoded_frame_queue_abort
							|| playback_state.load() == audio_playback_state_stopped
							|| decoded_frame_queue_samples < get_decoded_frame_queue_high_watermark();
					});
				if (decoded_frame_queue_abort || playback_state.load() == audio_playback_state_stopped)
					break;
				if (decoded_frame_queue_samples >= get_decoded_frame_queue_high_watermark())
					continue;
			}
		}

		if (playback_state.load() == audio_playback_state_init
			&& is_pause) {
			NATIVE_TRACE("info: resume from pause, pts_seconds=%lf\n", pts_seconds.load());
			if (av_seek_frame(format_context, -1, static_cast<int64_t>(pts_seconds * AV_TIME_BASE), AVSEEK_FLAG_ANY) < 0) {
				NATIVE_TRACE("err: resume failed\n");
				playback_state.store(audio_playback_state_stopped);
			}
			else
			{
				reset_audio_source_bitrate();
			}
			avcodec_flush_buffers(codec_context);
			is_pause = false;
			is_eof = false;
			decoder_flushed = false;
		}

		// 从输入文件中读取数据并解码
		if (!is_eof) {
			if (timed_decoder_call([this]
				{ return av_read_frame(format_context, packet); }) < 0) {
				NATIVE_TRACE("info: av_read_frame reached eof, entering flush mode\n");
				// 文件流结束，进入flush模式
				is_eof = true;
			}
			else if (packet->stream_index != audio_stream_index) {
				notify_frame_underrun();
				av_packet_unref(packet);
				decoder_pending_work_milliseconds = 0.0;
				continue; // 跳过非音频流包
			}
		}
		// is_eof后，不单独检查packet->stream_index。此时此值无意义。

		if (is_eof && !decoder_flushed) {
			// 发送空包以排空解码器缓存
			decoder_last_call_result = timed_decoder_call([this]
				{ return avcodec_send_packet(codec_context, nullptr); });
			if (decoder_last_call_result < 0 && decoder_last_call_result != AVERROR_EOF) {
				NATIVE_TRACE("warn: flush decoder failed, code=%d\n", decoder_last_call_result);
			}
			decoder_flushed = true;
		}
		else if (!is_eof) {
			// 正常送入数据包
			decoder_last_call_result = timed_decoder_call([this]
				{ return avcodec_send_packet(codec_context, packet); });
			if (decoder_last_call_result < 0) {
				if (decoder_last_call_result == AVERROR_INVALIDDATA) {
					// 忽略坏块
					observe_and_reset_decoder_pending_work(0, 0);
					av_packet_unref(packet);
					continue;
				}
				av_packet_unref(packet);
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(decoder_last_call_result);
				break;
			}
			observe_audio_source_packet(*packet);
		}
		while (true)
		{
			decoder_last_call_result = timed_decoder_call([this]
				{ return avcodec_receive_frame(codec_context, frame.get()); });
			if (decoder_last_call_result == AVERROR(EAGAIN)) {
				break; // 没有更多帧
			}
			if (decoder_last_call_result == AVERROR_EOF) {
				observe_and_reset_decoder_pending_work(0, 0);
				NATIVE_TRACE("info: decoder flushed, signaling normalizer eof\n");
				signal_decoded_frame_queue_eof();
				av_frame_unref(frame.get());
				if (!is_eof) {
					av_packet_unref(packet);
				}
				return;
			}
			if (decoder_last_call_result < 0) {
				av_frame_unref(frame.get());
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(decoder_last_call_result);
				break;
			}
			observe_audio_source_frame(*frame);
			observe_and_reset_decoder_pending_work(
				frame->nb_samples, frame->sample_rate);
			if (!queue_decoded_frame(frame.get()))
			{
				if (playback_state.load() != audio_playback_state_stopped)
				{
					NATIVE_TRACE("err: queue decoded frame failed\n");
					playback_state.store(audio_playback_state_stopped);
				}
				break;
			}
		}
		av_frame_unref(frame.get()); // eof, err process -> proper unref
		if (!is_eof) {
			// EOF模式时，发送的包是空包
			av_packet_unref(packet);
		}
	}

	if (playback_state.load() != audio_playback_state_stopped)
		signal_decoded_frame_queue_eof();
}

void MusicPlayerLibrary::AudioFile::audio_bit_perfect_worker_thread()
{
	while (playback_state.load() != audio_playback_state_stopped &&
		!user_request_stop.load())
	{
		if (!wait_for_audio_fifo_room())
		{
			break;
		}

		UniqueAvFrame decoded_frame(pop_decoded_frame());
		if (!decoded_frame)
		{
			bool reached_eof;
			{
				std::lock_guard queue_lock(decoded_frame_queue_mutex);
				reached_eof = decoded_frame_queue_eof &&
					decoded_frame_queue.empty() && !decoded_frame_queue_abort;
			}
			if (reached_eof)
			{
				if (normalized_stream_frames_.load(std::memory_order_acquire) == 0)
				{
					is_audio_file_and_sink_bit_perfect.store(
						false, std::memory_order_release);
				}
				NATIVE_TRACE(
					"info: decoded queue eof, bit-perfect stream completed without normalization\n");
				normalized_stream_end.store(true, std::memory_order_release);
				notify_observers_end_of_stream(
					audio_stream_generation_.load(std::memory_order_acquire));
				file_stream_end.store(true, std::memory_order_release);
			}
			break;
		}

		const AVChannelLayout& decoded_layout =
			decoded_frame->ch_layout.nb_channels > 0
				? decoded_frame->ch_layout
				: codec_context->ch_layout;
		const int decoded_sample_rate = decoded_frame->sample_rate > 0
			? decoded_frame->sample_rate
			: codec_context->sample_rate;
		const auto decoded_sample_format =
			static_cast<AVSampleFormat>(decoded_frame->format);
		const DecodedAudioFormat current_format = MakeDecodedAudioFormat(
			decoded_sample_rate,
			decoded_sample_format,
			decoded_layout,
			audio_source_format_info.bit_depth);
		if (!AreAudioFormatsBitPerfect(current_format, audio_output_format) ||
			decoded_frame->nb_samples <= 0 || !decoded_frame->extended_data ||
			!decoded_frame->extended_data[0])
		{
			throw std::runtime_error(
				"Decoded audio format changed after the bit-perfect handshake");
		}

		notify_observers_pcm_frame(*decoded_frame);

		{
			std::lock_guard fifo_lock(audio_fifo_mutex);
			if (const int ret_code = add_samples_to_fifo(
				decoded_frame->extended_data, decoded_frame->nb_samples);
				ret_code < 0)
			{
				FFMPEG_CRITICAL_ERROR(ret_code);
			}
		}
		notify_frame_ready();

		if (is_audio_sink_initialized() &&
			query_audio_sink_buffer_count() < 4 &&
			playback_state.load() == audio_playback_state_playing)
		{
			notify_frame_ready();
		}
	}
	notify_frame_ready();
	notify_frame_underrun();
}

void MusicPlayerLibrary::AudioFile::audio_normalize_worker_thread()
{
	auto drain_normalized_audio_to_fifo = [this]()
		{
			while (true)
			{
				const int res = av_buffersink_get_frame(
					normalization_sink_context, normalized_frame.get());
				if (res == AVERROR(EAGAIN))
					// 归一化滤镜已排空，从 decoded_frame_queue 取下一帧。
					return true;
				if (res == AVERROR_EOF)
				{
					NATIVE_TRACE("info: filter flushed, all samples processed\n");
					normalized_stream_end = true;
					notify_observers_end_of_stream(audio_stream_generation_.load());
					file_stream_end = true;
					return false;
				}
				if (res < 0)
				{
					FFMPEG_CRITICAL_ERROR(res);
					playback_state.store(audio_playback_state_stopped);
					return false;
				}

				notify_observers_pcm_frame(*normalized_frame);

				{
					std::lock_guard fifo_lock(audio_fifo_mutex);
					if (const int ret_code = add_samples_to_fifo(
						normalized_frame->extended_data, normalized_frame->nb_samples);
						ret_code < 0)
					{
						av_frame_unref(normalized_frame.get());
						playback_state.store(audio_playback_state_stopped);
						FFMPEG_CRITICAL_ERROR(ret_code);
						return false;
					}
				}
				av_frame_unref(normalized_frame.get());
				notify_frame_ready();
			}
		};

	bool filter_flush_sent = false;
	while (playback_state.load() != audio_playback_state_stopped && !user_request_stop.load())
	{
		if (!wait_for_audio_fifo_room())
			break;

		UniqueAvFrame decoded_frame(pop_decoded_frame());
		if (!decoded_frame)
		{
			bool reached_eof;
			{
				std::lock_guard queue_lock(decoded_frame_queue_mutex);
				reached_eof = decoded_frame_queue_eof
					&& decoded_frame_queue.empty()
					&& !decoded_frame_queue_abort;
			}

			if (!reached_eof)
				break;

			std::lock_guard graph_lock(filter_graph_mutex);
			if (!filter_flush_sent)
			{
				NATIVE_TRACE("info: decoded queue eof, flushing audio filter graph\n");
				if (const int ret = av_buffersrc_add_frame(
					normalization_source_context, nullptr); ret < 0 && ret != AVERROR_EOF)
				{
					FFMPEG_CRITICAL_ERROR(ret);
					playback_state.store(audio_playback_state_stopped);
					break;
				}
				filter_flush_sent = true;
			}
			if (!drain_normalized_audio_to_fifo())
				break;
			std::this_thread::sleep_for(PlaybackPollInterval);
			continue;
		}

		const int normalization_samples = decoded_frame->nb_samples;
		const int normalization_sample_rate = decoded_frame->sample_rate > 0
			? decoded_frame->sample_rate
			: (codec_context && codec_context->sample_rate > 0
				? codec_context->sample_rate
				: audio_output_format.sample_rate);
		const auto normalization_begin = std::chrono::steady_clock::now();
		bool normalization_succeeded = true;
		{
			std::lock_guard graph_lock(filter_graph_mutex);
			if (int add_frame_ret = av_buffersrc_add_frame(
				normalization_source_context, decoded_frame.get()); add_frame_ret < 0)
			{
				decoded_frame.reset();
				playback_state.store(audio_playback_state_stopped);
				if (add_frame_ret != AVERROR_EOF) {
					FFMPEG_CRITICAL_ERROR(add_frame_ret);
				}
				else {
					NATIVE_TRACE("info: normalization filter shutdown, exiting\n");
				}
				normalization_succeeded = false;
			}
			else
			{
				decoded_frame.reset();
				normalization_succeeded = drain_normalized_audio_to_fifo();
			}
		}
		observe_audio_pipeline_stage_duration(
			audio_pipeline_stage::normalizer,
			ElapsedMilliseconds(
				std::chrono::steady_clock::now() - normalization_begin),
			normalization_samples,
			normalization_sample_rate);
		if (!normalization_succeeded)
			break;


		bool decoded_queue_has_room;
		{
			std::lock_guard queue_lock(decoded_frame_queue_mutex);
			decoded_queue_has_room = !decoded_frame_queue_abort
				&& decoded_frame_queue_samples < get_decoded_frame_queue_high_watermark();
		}
		if (decoded_queue_has_room)
			decoded_frame_queue_cv.notify_all();

		int player_buffers_queued = is_audio_sink_initialized()
			                             ? query_audio_sink_buffer_count()
			                             : 0;
		if (player_buffers_queued < 4 && playback_state.load() == audio_playback_state_playing) {
			NATIVE_TRACE("info: xaudio2 buffers queued=%d, notify player thread to submit data\n", player_buffers_queued);
			notify_frame_ready();
		}
	}
	notify_frame_ready();
	notify_frame_underrun();
}

void MusicPlayerLibrary::AudioFile::handle_worker_exception(const std::string& message, const char* worker_name)
{
	NATIVE_TRACE("err: audio %s worker failed\n", worker_name);
	is_audio_file_and_sink_bit_perfect.store(false, std::memory_order_release);
	playback_state.store(audio_playback_state_stopped);
	user_request_stop.store(true);
	decoder_is_running = false;
	normalizer_is_running = false;
	abort_decoded_frame_queue_waiters();
	if (audio_sink_)
		audio_sink_->AbortStream();

	publish_message({PlayerMessageType::Error, message});
}

void MusicPlayerLibrary::AudioFile::audio_normalizer_dispatch_worker_thread()
{
	if (is_audio_file_and_sink_bit_perfect.load(std::memory_order_acquire))
		audio_bit_perfect_worker_thread();
	else
		audio_normalize_worker_thread();
}

void MusicPlayerLibrary::AudioFile::run_scheduled_audio_worker(
	const wchar_t* task_name,
	const MPL_AUDIO_PRIORITY priority,
	const char* worker_name,
	const AudioWorker worker)
{
	auto thread_schedule = CreateDefaultAudioThreadScheduleHelper(
		task_name, priority, worker_name);
	if (!thread_schedule)
	{
		NATIVE_TRACE(
			"warn: Audio thread scheduler factory is not implemented on this system");
	}

	try
	{
		(this->*worker)();
	}
	catch (const std::exception& exception)
	{
		handle_worker_exception(
			std::format("Unhandled native exception in {} worker: {}",
				worker_name, exception.what()),
			worker_name);
	}
	catch (...)
	{
		handle_worker_exception(
			std::format("Unhandled unknown exception in {} worker.", worker_name),
			worker_name);
	}
}



void MusicPlayerLibrary::AudioFile::init_decoder_thread() {
	if (audio_decoder_worker_thread.joinable() || audio_normalizer_worker_thread.joinable())
	{
		stop_audio_decode();
	}
	file_stream_end = false;
	reset_decoded_frame_queue(false);
	init_normalizer_thread();
	decoder_is_running = true;
	audio_decoder_worker_thread = std::jthread(
		[this] {
			run_scheduled_audio_worker(
				AudioMmcssTaskName, MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_HIGH,
				"decoder", &AudioFile::audio_decode_worker_thread);
			decoder_is_running = false;
		});
	NATIVE_TRACE("info: decoder thread created\n");
	notify_frame_underrun();
}

void MusicPlayerLibrary::AudioFile::init_normalizer_thread()
{
	if (audio_normalizer_worker_thread.joinable())
	{
		stop_audio_normalizer();
	}
	normalizer_is_running = true;
	audio_normalizer_worker_thread = std::jthread(
		[this] {
			run_scheduled_audio_worker(
				AudioMmcssTaskName, MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_HIGH,
				"normalizer", &AudioFile::audio_normalizer_dispatch_worker_thread);
			normalizer_is_running = false;
		});
	NATIVE_TRACE("info: audio normalizer thread created\n");
}

inline void MusicPlayerLibrary::AudioFile::start_audio_playback()
{
	const bool restarting_stopped_stream =
		playback_state.load() == audio_playback_state_stopped;
	if (!restarting_stopped_stream && audio_player_worker_thread.joinable())
	{
		// A first Start can still be buffering in the init state. Do not try to
		// join that live worker when Start is invoked again.
		return;
	}
	if (restarting_stopped_stream) {
		reset_audio_context();
		if (playback_state.load() == audio_playback_state_stopped)
		{
			NATIVE_TRACE("err: audio context reset failed, playback will not start\n");
			return;
		}
	}
	playback_state.store(audio_playback_state_init);
	message_interval_timer = -1.0f;
	if (audio_player_worker_thread.joinable())
	{
		audio_player_worker_thread.join();
	}
	user_request_stop.store(false);
	audio_player_worker_thread = std::jthread(
		[this] {
			run_scheduled_audio_worker(
				AudioMmcssTaskName, MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_HIGH,
				"playback", &AudioFile::audio_playback_worker_thread);
		});
	NATIVE_TRACE("info: player thread created\n");
	// notify decoder to start decoding
}

void MusicPlayerLibrary::AudioFile::stop_audio_decode(int mode)
{
	if (audio_decoder_worker_thread.joinable() || audio_normalizer_worker_thread.joinable())
	{
		playback_state.store(audio_playback_state_stopped);
		abort_decoded_frame_queue_waiters();
		if (audio_decoder_worker_thread.joinable())
		{
			audio_decoder_worker_thread.request_stop();
			audio_decoder_worker_thread.join();
		}
		stop_audio_normalizer();
	}
	decoder_is_running = false;
	normalizer_is_running = false;
}

void MusicPlayerLibrary::AudioFile::stop_audio_normalizer()
{
	if (audio_normalizer_worker_thread.joinable())
	{
		abort_decoded_frame_queue_waiters();
		audio_normalizer_worker_thread.request_stop();
		audio_normalizer_worker_thread.join();
	}
	normalizer_is_running = false;
	reset_decoded_frame_queue(true);
}

void MusicPlayerLibrary::AudioFile::stop_audio_playback(int mode)
{
	if (audio_player_worker_thread.joinable())
	{
		// 播放线程在排空fifo时可能长期持有锁，此处持有锁会导致暂停时ui冻结。
		// 使用std::atomic_bool存储标志位，并在播放线程的几个关键节点手动检测。
		// *重构的时候才想起TryEnterCriticalSection的好。
		user_request_stop.store(true);
		playback_state.store(audio_playback_state_stopped);
		notify_all_frame_notifications();
		audio_player_worker_thread.request_stop();
		audio_player_worker_thread.join();

	}
	if (audio_sink_)
		audio_sink_->AbortStream();
	// Stop decoder after playback thread exits; pause/reset can then rebuild FIFO
	// and filters without racing the playback worker.
	stop_audio_decode(is_pause ? 1 : 0);
	double pts_time_d;
	if (is_pause)
	{
		pts_time_d = pts_seconds.load();
	}
	else {
		elapsed_time = pts_time_d = 0.0;
	}
	suppress_time_events = false;
	publish_message({PlayerMessageType::TimeChanged, pts_time_d});
	// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
	reset_frame_notifications();
	if (mode == 0)
		reset_audio_context();
	else if (mode == -1)
		release_audio_context();
}

int MusicPlayerLibrary::AudioFile::initialize_audio_fifo(AVSampleFormat sample_fmt, int channels, int nb_samples)
{
	audio_fifo.reset(av_audio_fifo_alloc(sample_fmt, channels, nb_samples));
	if (!audio_fifo)
	{
		// AfxMessageBox(_T("err: could not allocate audio fifo!"), MB_ICONERROR);
		FFMPEG_CRITICAL_ERROR(-1);
		return -1;
	}
	return 0;
}

int MusicPlayerLibrary::AudioFile::resize_audio_fifo(int nb_samples)
{
	if (!audio_fifo)
		return -1;
	if (int ret_value; (ret_value = av_audio_fifo_realloc(audio_fifo.get(), nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret_value);
		return ret_value;
	}
	return 0;
}

int MusicPlayerLibrary::AudioFile::add_samples_to_fifo(uint8_t** decoded_data, int nb_samples)
{
	if (!audio_fifo)
		return -1;
	if (int res = av_audio_fifo_write(audio_fifo.get(), reinterpret_cast<void**>(decoded_data), nb_samples); res < 0) {
		// audio fifo will resize automatically
		FFMPEG_CRITICAL_ERROR(res);
		return res;
	}
	// 	ATLTRACE("info: added %d samples to audio fifo\n", res);
	return 0;
}

int MusicPlayerLibrary::AudioFile::read_samples_from_fifo(uint8_t** output_buffer, int nb_samples)
{
	int ret;
	if (!audio_fifo)
		return -1;
	if ((ret = av_audio_fifo_read(audio_fifo.get(), reinterpret_cast<void**>(output_buffer), nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret);
		return -1;
	}
	return ret;
}

void MusicPlayerLibrary::AudioFile::drain_audio_fifo(int nb_samples)
{
	if (!audio_fifo)
		return;
	if (int ret; (ret = av_audio_fifo_drain(audio_fifo.get(), nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret);
	}
}

void MusicPlayerLibrary::AudioFile::reset_audio_fifo()
{
	if (!audio_fifo)
		return;
	av_audio_fifo_reset(audio_fifo.get());
}

int MusicPlayerLibrary::AudioFile::get_audio_fifo_cached_samples_size()
{
	if (!audio_fifo)
		return -1;
	return av_audio_fifo_size(audio_fifo.get());
}

void MusicPlayerLibrary::AudioFile::uninitialize_audio_fifo()
{
	audio_fifo.reset();
}

inline const char* MusicPlayerLibrary::AudioFile::get_backend_implement_version() // NOLINT(*-convert-member-functions-to-static)
{
	static char backend_implement_version[] = "XAudio 2.8 Compatible";
	return backend_implement_version;
}

int MusicPlayerLibrary::AudioFile::query_audio_sink_buffer_count()
{
	return audio_sink_
		? static_cast<int>(audio_sink_->GetState().buffers_queued)
		: 0;
}

bool MusicPlayerLibrary::AudioFile::is_audio_sink_initialized()
{
	return audio_sink_ && audio_sink_->IsInitialized();
}

void MusicPlayerLibrary::AudioFile::dialog_ffmpeg_critical_error(int err_code, const char* file, int line) // NOLINT(*-convert-member-functions-to-static)
{
	std::string detail = std::format(
		"FFmpeg critical error: {} (file: {}, line: {})\n",
		GetFFmpegErrorMessage(err_code), file, line);
	throw std::runtime_error(detail);
}

MusicPlayerLibrary::AudioFile::AudioFile(
	IMusicPlayerMessageSink* message_sink,
	std::optional<AudioOutputFormat> output_format,
	std::shared_ptr<IAudioSink> sink) :
	playback_state(audio_playback_state_init),
	initial_buffering_profile(GetAudioPipelineBufferingProfile()),
	message_sink_(message_sink)
{
	const AudioOutputFormat requested = output_format.value_or(AudioOutputFormat{});
	Connect(sink ? std::move(sink) : CreateAudioSink(requested));
	fft_observer_ = std::make_shared<FFTAudioObserve>(audio_output_format);
	Subscribe(fft_observer_);
	NATIVE_TRACE("info: decode frontend: avformat version %d, avcodec version %d, avutil version %d, avfilter version %d\n",
		avformat_version(),
		avcodec_version(),
		avutil_version(),
		avfilter_version());
	NATIVE_TRACE(
		"info: audio api backend: %s%s\n",
		audio_sink_->IsExclusiveModeEnabled()
			? "WASAPI exclusive"
			: "FAudio shared, version ",
		audio_sink_->IsExclusiveModeEnabled()
			? ""
			: get_backend_implement_version());
	NATIVE_TRACE("info: audio output format: sample_rate=%d, channels=%u, channel_mask=0x%x, bits=%d, ffmpeg_layout=%s\n",
		audio_output_format.sample_rate,
		audio_output_format.channel_count,
		audio_output_format.channel_mask,
		static_cast<int>(audio_output_format.bit_depth),
		audio_output_format.ffmpeg_channel_layout.c_str());
}

void MusicPlayerLibrary::AudioFile::Connect(std::shared_ptr<IAudioSink> sink)
{
	if (!sink || !sink->IsInitialized())
		throw std::invalid_argument("AudioFile requires an initialized audio sink");
	if (is_audio_context_initialized() || audio_player_worker_thread.joinable() ||
		audio_decoder_worker_thread.joinable() ||
		audio_normalizer_worker_thread.joinable())
	{
		throw std::logic_error(
			"AudioFile can only connect a sink while the file is closed");
	}
	if (audio_sink_ && audio_sink_ != sink)
		audio_sink_->AbortStream();
	audio_sink_ = std::move(sink);
	audio_output_format = audio_sink_->GetOutputFormat();
	const auto block_align = (std::max)(
		static_cast<std::uint32_t>(audio_output_format.wave_format.Format.nBlockAlign),
		std::uint32_t{1});
	const auto maximum_submit_frames = (std::min)(
		static_cast<std::uint32_t>(
			(std::numeric_limits<int>::max)() / 8),
		(std::numeric_limits<std::uint32_t>::max)() / block_align);
	const auto preferred_submit_frames = (std::min)(
		audio_sink_->GetPreferredSubmitFrameCount(),
		maximum_submit_frames);
	sink_submit_frame_size = (std::max)(
		MinimumSinkSubmitFrameCount,
		static_cast<int>(preferred_submit_frames));
	is_audio_file_and_sink_bit_perfect.store(false, std::memory_order_release);
}

void MusicPlayerLibrary::AudioFile::Subscribe(std::shared_ptr<IAudioObserve> observe)
{
	if (!observe)
		return;

	// Observer delivery and registration share a serialization boundary. This
	// guarantees that a late subscriber receives its format and generation
	// before it can appear in a PCM-delivery snapshot.
	std::lock_guard dispatch_lock(audio_observer_dispatch_mutex_);
	{
		std::lock_guard observers_lock(audio_observers_mutex_);
		if (std::find(audio_observers_.begin(), audio_observers_.end(), observe) !=
			audio_observers_.end())
		{
			return;
		}
	}

	const auto generation = audio_stream_generation_.load();
	if (generation != 0)
	{
		observe->OnFormat(audio_output_format, generation);
		observe->OnReset(generation);
	}

	std::lock_guard observers_lock(audio_observers_mutex_);
	audio_observers_.push_back(std::move(observe));
}

void MusicPlayerLibrary::AudioFile::ClearObservers()
{
	std::lock_guard dispatch_lock(audio_observer_dispatch_mutex_);
	std::lock_guard lock(audio_observers_mutex_);
	audio_observers_.clear();
	fft_observer_.reset();
}

const MusicPlayerLibrary::AudioOutputFormat&
MusicPlayerLibrary::AudioFile::GetNormalizedFormat() const noexcept
{
	return audio_output_format;
}

/**
 * @brief Initializes the audio normalization graph using libavfilter
 *
 * @note The source graph only resamples and converts channel/sample formats.
 * Pre-gain, equalization, and limiting are owned by the audio sink.
 */
int MusicPlayerLibrary::AudioFile::init_audio_normalization_filter()
{
	std::lock_guard graph_lock(filter_graph_mutex);
	clear_audio_normalization_filter_locked();
	normalization_filter_graph.reset(avfilter_graph_alloc());
	if (!normalization_filter_graph)
	{
		NATIVE_TRACE("err: avfilter_graph_alloc failed\n");
		return -1;
	}
	auto release_normalization_filter = [this]() noexcept {
		clear_audio_normalization_filter_locked();
	};
	auto fail_filter = [&release_normalization_filter](int code) {
		release_normalization_filter();
		return code;
		};
	auto fail_filter_with_ffmpeg = [&release_normalization_filter, this](int code) {
		release_normalization_filter();
		FFMPEG_CRITICAL_ERROR(code);
		return code;
		};

	std::string layout_str(256, '\0');
	av_channel_layout_describe(&codec_context->ch_layout, layout_str.data(), layout_str.size());
	layout_str.resize(strlen(layout_str.c_str()));
	const char* sample_fmt_name = av_get_sample_fmt_name(codec_context->sample_fmt);
	std::string args = std::format("sample_rate={}:sample_fmt={}:channel_layout={}",
		codec_context->sample_rate,
		sample_fmt_name == nullptr ? "" : sample_fmt_name,
		layout_str);
	NATIVE_TRACE("info: init_audio_normalization_filter, filter args: %s\n", args.c_str());
	int ret = avfilter_graph_create_filter(
		&normalization_source_context,
		avfilter_get_by_name("abuffer"),
		"src", args.c_str(), nullptr, normalization_filter_graph.get());
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}

	const char* output_sample_fmt_name = av_get_sample_fmt_name(audio_output_format.sample_format);
	if (!output_sample_fmt_name)
		return fail_filter(-1);
	std::string resample_args = std::format(
		"sample_rate={}:out_chlayout={}:out_sample_fmt={}",
		audio_output_format.sample_rate,
		audio_output_format.ffmpeg_channel_layout,
		output_sample_fmt_name);
	ret = avfilter_graph_create_filter(
		&resample_context,
		avfilter_get_by_name("aresample"),
		"resample", resample_args.c_str(), nullptr, normalization_filter_graph.get());
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	NATIVE_TRACE("info: resample filter created, param = %s\n", resample_args.c_str());

	ret = avfilter_graph_create_filter(
		&normalization_sink_context,
		avfilter_get_by_name("abuffersink"),
		"sink", nullptr, nullptr, normalization_filter_graph.get());
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	std::string fmt_args = std::format(
		"sample_fmts={}:sample_rates={}:channel_layouts={}",
		output_sample_fmt_name,
		audio_output_format.sample_rate,
		audio_output_format.ffmpeg_channel_layout);
	ret = avfilter_graph_create_filter(&format_normalization_context,
		avfilter_get_by_name("aformat"),
		"aformat", fmt_args.c_str(), nullptr, normalization_filter_graph.get());
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	NATIVE_TRACE("info: format filter created, param = %s\n", fmt_args.c_str());
	if ((ret = avfilter_link(normalization_source_context, 0, resample_context, 0)) < 0 ||
		(ret = avfilter_link(resample_context, 0, format_normalization_context, 0)) < 0 ||
		(ret = avfilter_link(format_normalization_context, 0, normalization_sink_context, 0)) < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}

	ret = avfilter_graph_config(normalization_filter_graph.get(), nullptr);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}

	const AVFilterLink* sink_link = normalization_sink_context->inputs[0];
	const int sink_channels = sink_link->ch_layout.nb_channels;
	NATIVE_TRACE("info: filter output format=%s, sample_rate=%d, channels=%d\n",
		av_get_sample_fmt_name(static_cast<AVSampleFormat>(sink_link->format)),
		sink_link->sample_rate,
		sink_channels);
	if (sink_link->sample_rate != audio_output_format.sample_rate ||
		sink_channels != fifo_audio_channels ||
		sink_link->format != fifo_audio_sample_fmt)
	{
		NATIVE_TRACE("err: filter output format mismatch, expected format=%s, sample_rate=%d, channels=%d\n",
			output_sample_fmt_name, audio_output_format.sample_rate, fifo_audio_channels);
		return fail_filter(-1);
	}
	return 0;
}

bool MusicPlayerLibrary::AudioFile::is_audio_normalization_filter_initialized()
{
	return normalization_source_context && normalization_sink_context;
}

void MusicPlayerLibrary::AudioFile::clear_audio_normalization_filter_locked() noexcept
{
	normalization_filter_graph.reset();
	normalization_source_context = normalization_sink_context = nullptr;
	resample_context = nullptr;
	format_normalization_context = nullptr;
}

void MusicPlayerLibrary::AudioFile::reset_audio_normalization_filter()
{
	std::lock_guard graph_lock(filter_graph_mutex);
	clear_audio_normalization_filter_locked();
}

bool MusicPlayerLibrary::AudioFile::IsInitialized()
{
	return is_audio_context_initialized() && is_audio_sink_initialized();
}

bool MusicPlayerLibrary::AudioFile::IsPlaying()
{
	return IsInitialized() &&
		playback_state.load() != audio_playback_state_init && playback_state.load() != audio_playback_state_stopped;
}

void MusicPlayerLibrary::AudioFile::OpenFile(const std::wstring& fileName, const std::wstring& file_extension_in, bool skip_album_art_loading)
{
	if (is_audio_context_initialized() || audio_player_worker_thread.joinable() ||
		audio_decoder_worker_thread.joinable() || audio_normalizer_worker_thread.joinable())
	{
		user_request_stop.store(true);
		release_audio_session_resources();
	}
	user_request_stop.store(false);
	is_pause.store(false);
	pts_seconds.store(0.0);
	elapsed_time.store(0.0);
	playback_state.store(audio_playback_state_init);
	reset_frame_notifications();

	try
	{
		if (load_audio_context(fileName, file_extension_in, skip_album_art_loading)) {
			// AfxMessageBox(_T("err: load file failed, please check trace message!"), MB_ICONERROR);
			throw std::runtime_error("Load file failed, please re-run in terminal and check trace message!");
		}
		if (initialize_audio_session()) {
			// AfxMessageBox(_T("err: audio engine initialize failed!"), MB_ICONERROR);
			throw std::runtime_error("Audio engine initialize failed!");
		};
		publish_message({PlayerMessageType::FileInitialized});
		publish_message({PlayerMessageType::TimeChanged, 0.0});
		// AfxGetMainWnd()->PostMessage(WM_PLAYER_FILE_INIT);
	}
	catch (...)
	{
		release_audio_session_resources();
		throw;
	}
}

void MusicPlayerLibrary::AudioFile::Close()
{
	user_request_stop.store(true);
	playback_state.store(audio_playback_state_stopped);
	release_audio_session_resources();
	user_request_stop.store(false);
	is_pause.store(false);
	pts_seconds.store(0.0);
	elapsed_time.store(0.0);
	normalized_stream_end.store(false);
	playback_state.store(audio_playback_state_init);
	reset_frame_notifications();
}

double MusicPlayerLibrary::AudioFile::GetMusicTimeLength()
{
	if (!IsInitialized())
		return 0.0;
	double duration = length.load(std::memory_order_relaxed);
	if (duration <= 0.0)
	{
		duration = GetAudioStreamDurationSeconds(format_context, audio_stream_index);
		length.store(duration, std::memory_order_relaxed);
	}
	return duration;
}

double MusicPlayerLibrary::AudioFile::GetCurrentMusicPosition()
{
	if (IsInitialized())
	{
		return elapsed_time.load();
	}
	return 0.0;
}

std::wstring MusicPlayerLibrary::AudioFile::GetSongTitle()
{
	if (IsInitialized()) {
		return song_title;
	}
	return {};
}

std::wstring MusicPlayerLibrary::AudioFile::GetSongArtist()
{
	if (IsInitialized()) {
		return song_artist;
	}
	return {};
}

MusicPlayerLibrary::AudioFormatInfo
MusicPlayerLibrary::AudioFile::GetAudioSourceFormatInfo() const noexcept
{
	return audio_source_format_info;
}

MusicPlayerLibrary::AudioFormatInfo
MusicPlayerLibrary::AudioFile::GetDeviceOutputFormatInfo() const noexcept
{
	return GetAudioFormatInfo(audio_output_format);
}

MusicPlayerLibrary::AudioFormatInfo
MusicPlayerLibrary::AudioFile::GetSharedDeviceOutputFormatInfo() const noexcept
{
	return audio_sink_
		? GetAudioFormatInfo(audio_sink_->GetDeviceFormat())
		: AudioFormatInfo{};
}

double MusicPlayerLibrary::AudioFile::GetAudioSourceBitrate() const noexcept
{
	return audio_source_bitrate_tracker.GetKBytesPerSecond();
}

bool MusicPlayerLibrary::AudioFile::IsLoselessAudio() const noexcept
{
	return is_loseless_audio.load(std::memory_order_relaxed);
}

bool MusicPlayerLibrary::AudioFile::IsHiResAudio() const noexcept
{
	return is_hi_res_audio.load(std::memory_order_relaxed);
}

bool MusicPlayerLibrary::AudioFile::IsBitPerfect() const noexcept
{
	return is_audio_file_and_sink_bit_perfect.load(std::memory_order_acquire) &&
		audio_sink_ && audio_sink_->IsExclusiveModeEnabled() &&
		!audio_sink_->IsLimiterEnabled();
}

int MusicPlayerLibrary::AudioFile::GetAverageAudioBitrate() const noexcept
{
	return get_average_audio_bitrate();
}

void MusicPlayerLibrary::AudioFile::Start()
{
	if (IsInitialized() && !IsPlaying()) {
		start_audio_playback();
	}
}

void MusicPlayerLibrary::AudioFile::Stop()
{
	if (IsInitialized() && IsPlaying()) {
		pts_seconds = 0;
		stop_audio_playback(0);
		publish_message({PlayerMessageType::Stopped});
	}
}

void MusicPlayerLibrary::AudioFile::SetMasterVolume(float volume)
{
	if (audio_sink_)
		audio_sink_->SetMasterVolume(volume);
}

void MusicPlayerLibrary::AudioFile::SeekToPosition(double time, bool need_stop)
{
	if (IsInitialized()) {
		is_pause = true;
		pts_seconds = time;
		if (IsInitialized())
		{
			if (need_stop && (IsPlaying() || audio_player_worker_thread.joinable()))
			{
				suppress_time_events = true;
				user_request_stop.store(true);
				stop_audio_playback(0);
			}
			else if (!IsPlaying()) {
				if (decoder_is_running) {
					stop_audio_decode(1);
					playback_state.store(audio_playback_state_init);
					reset_audio_context();
					publish_message({PlayerMessageType::TimeChanged, time});
				}
			}
		}
	}
}

// 根据奈奎斯特采样定律，建议选择44.1kHz以上的采样率获得完整听觉体验
void MusicPlayerLibrary::AudioFile::SetSampleRate(int sample_rate)
{
	if (IsInitialized()) {
		// Set sample rate after init is not supported.
		throw std::logic_error("SetSampleRate is not supported after initialization!");
	}
	AudioOutputFormat requested = audio_output_format;
	requested.requested_sample_rate = sample_rate;
	Connect(CreateAudioSink(requested));
}

int MusicPlayerLibrary::AudioFile::GetNBlockAlign()
{
	return audio_output_format.wave_format.Format.nBlockAlign;
}

std::wstring MusicPlayerLibrary::AudioFile::GetID3Lyric()
{
	return id3_string_lyric;
}

int MusicPlayerLibrary::AudioFile::CopyAudioFFTData(
	float* destination,
	const int destination_length) const
{
	std::shared_ptr<FFTAudioObserve> observer;
	{
		std::lock_guard lock(audio_observers_mutex_);
		observer = fft_observer_;
	}
	return observer
		? [&]()
			{
				const auto state = audio_sink_
					? audio_sink_->GetState()
					: AudioSinkState{};
				const auto generation = audio_stream_generation_.load(
					std::memory_order_acquire);
				if (state.stream_ended || state.generation != generation)
					return 0;
				return observer->CopySpectrum(
					destination,
					destination_length,
					generation,
					state.media_frames_presented);
			}()
		: 0;
}

int MusicPlayerLibrary::AudioFile::GetEqualizerBand(int index)
{
	return audio_sink_ ? audio_sink_->GetEqualizerBand(index) : 0;
}

void MusicPlayerLibrary::AudioFile::SetEqualizerBand(int index, int value)
{
	if (audio_sink_)
		audio_sink_->SetEqualizerBand(index, value);
}

void MusicPlayerLibrary::AudioFile::Pause()
{
	if (IsInitialized() && IsPlaying()) {
		is_pause = true;
		pts_seconds = elapsed_time.load();
		stop_audio_playback(0);
		publish_message({PlayerMessageType::Paused});
	}
}

MusicPlayerLibrary::AudioFile::~AudioFile()
{
	if (playback_state.load() == audio_playback_state_playing) {
		user_request_stop.store(true);
		stop_audio_playback(-1);
	}
	stop_audio_decode();
	release_audio_session_resources();
}

#if defined(FFMPEG_CRITICAL_ERROR)
#undef FFMPEG_CRITICAL_ERROR
#endif
