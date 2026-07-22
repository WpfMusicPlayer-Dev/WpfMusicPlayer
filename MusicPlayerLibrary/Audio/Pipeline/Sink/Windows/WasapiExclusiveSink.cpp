// SPDX-License-Identifier: MIT

#include "pch.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <audioclient.h>
#include <mmreg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "Audio/DSP/EqualizerDsp.h"
#include "Audio/DSP/EqualizerSettings.h"
#include "Audio/DSP/PcmSampleConversion.h"
#include "Audio/Pipeline/Device/Windows/WasapiExclusiveOutputDevice.h"
#include "Audio/Pipeline/Sink/Windows/WasapiExclusiveSink.h"
#include "Audio/Pipeline/Windows/WasapiAudioHelpers.h"
#include "Core/AudioThreadScheduleHelper.h"
#include "Core/NumericConversion.h"
#include "Platform/Windows/ComPtr.h"
#include "Platform/Windows/WindowsResource.h"

namespace
{
	constexpr REFERENCE_TIME FallbackDevicePeriod = 100'000; // 10 ms
	constexpr auto CommandWaitTimeout = std::chrono::seconds(5);
}

struct MusicPlayerLibrary::WasapiExclusiveSink::Impl final
{
	enum class StreamPhase : std::uint8_t
	{
		idle,
		open,
		draining,
		eos_queued,
		ended,
		aborted,
		error
	};

	enum class CommandType : std::uint8_t
	{
		start,
		stop,
		reset,
		shutdown
	};

	struct QueuedBlock
	{
		std::vector<std::uint8_t> bytes;
		std::uint32_t frame_count = 0;
		std::uint32_t consumed_frames = 0;
		AudioStreamGeneration generation = 0;
	};

	struct ClockRun
	{
		std::uint64_t device_start = 0;
		std::uint32_t device_frames = 0;
		std::uint64_t media_start = 0;
		bool contains_media = false;
	};

	struct Command
	{
		CommandType type = CommandType::stop;
		std::uint64_t serial = 0;
	};

	std::shared_ptr<WasapiExclusiveOutputDevice> device;
	AudioOutputFormat output_format{};
	WAVEFORMATEXTENSIBLE wave_format{};

	Microsoft::WRL::ComPtr<IAudioClient> audio_client;
	Microsoft::WRL::ComPtr<IAudioRenderClient> render_client;
	Microsoft::WRL::ComPtr<IAudioClock> audio_clock;
	UniqueHandle render_event;
	UniqueHandle command_event;
	std::uint32_t buffer_frame_count = 0;
	std::uint32_t block_align = 0;
	std::uint64_t clock_frequency = 0;
	std::uint64_t clock_origin_position = 0;
	bool clock_origin_valid = false;
	WasapiClockHealthMonitor clock_health_monitor;
	DWORD render_wait_timeout_milliseconds = 50;
	std::uint32_t consecutive_render_timeouts = 0;
	std::uint32_t consecutive_buffer_errors = 0;
	std::uint32_t consecutive_media_underruns = 0;
	std::uint64_t recovery_count = 0;
	std::chrono::steady_clock::time_point last_render_service_time{};
	bool has_render_service_time = false;

	std::mutex lifecycle_mutex;
	mutable std::mutex state_mutex;
	std::condition_variable stream_end_cv;
	std::deque<QueuedBlock> queue;
	std::deque<ClockRun> clock_runs;
	StreamPhase phase = StreamPhase::idle;
	AudioStreamGeneration generation = 0;
	AudioStreamGeneration completed_generation = 0;
	std::uint32_t error_code = 0;
	std::uint32_t last_padding = 0;
	std::uint64_t submitted_media_frames = 0;
	std::uint64_t media_frames_written = 0;
	std::uint64_t presented_media_frames = 0;
	std::uint64_t total_device_frames_written = 0;
	std::uint64_t consumed_device_frames = 0;
	std::uint64_t drain_target_device_frames = 0;
	bool drain_output_finished = false;

	mutable std::mutex effect_mutex;
	AudioDsp::EqualizerSettings equalizer_settings;
	AudioDsp::EqualizerDspSnapshot equalizer_snapshot{};
	float master_volume = 1.0f;
	float published_master_volume = 1.0f;
	std::atomic_bool processing_enabled{false};
	AudioDsp::EqualizerDsp equalizer_dsp;
	AudioDsp::EqualizerDsp staged_equalizer_dsp;
	std::vector<std::uint8_t> byte_scratch;
	std::vector<std::uint8_t> render_scratch;
	std::vector<float> float_input;
	std::vector<float> float_output;

	std::mutex command_invoke_mutex;
	std::mutex command_mutex;
	std::condition_variable command_cv;
	std::deque<Command> commands;
	std::uint64_t next_command_serial = 0;
	std::uint64_t completed_command_serial = 0;
	HRESULT completed_command_result = E_FAIL;

	std::mutex initialization_mutex;
	std::condition_variable initialization_cv;
	bool initialization_complete = false;
	HRESULT initialization_result = E_FAIL;
	std::atomic_bool initialized{false};
	std::atomic_bool worker_running{false};
	std::atomic_bool started{false};
	bool needs_initial_prefill = true;
	std::jthread render_worker;

	explicit Impl(
		const AudioOutputFormat& requested,
		std::shared_ptr<WasapiExclusiveOutputDevice> selected_device) :
		device(selected_device
			? std::move(selected_device)
			: WasapiExclusiveOutputDevice::Acquire(requested)),
		render_event(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
		command_event(CreateEventW(nullptr, FALSE, FALSE, nullptr))
	{
		if (!device)
			throw std::runtime_error("WASAPI exclusive output device is unavailable");
		if (!render_event || !command_event)
			throw std::runtime_error(FormatHResult(
				"CreateEventW", HRESULT_FROM_WIN32(GetLastError())));

		output_format = device->ResolveSinkFormat(requested);
		wave_format = ToWindowsWaveFormatExtensible(output_format);
		block_align = wave_format.Format.nBlockAlign;
		{
			std::lock_guard effect_lock(effect_mutex);
			PublishEffectSnapshotLocked(true);
		}

		try
		{
			render_worker = std::jthread(
				[this](const std::stop_token stop_token)
				{
					RenderThread(stop_token);
				});

			std::unique_lock initialization_lock(initialization_mutex);
			initialization_cv.wait(initialization_lock, [this]
				{
					return initialization_complete;
				});
			if (FAILED(initialization_result))
			{
				const auto result = initialization_result;
				initialization_lock.unlock();
				StopWorkerAfterFailedInitialization();
				throw std::runtime_error(FormatHResult(
					"WASAPI exclusive initialization", result));
			}
		}
		catch (...)
		{
			StopWorkerAfterFailedInitialization();
			throw;
		}
	}

	~Impl()
	{
		if (render_worker.joinable())
		{
			if (worker_running.load(std::memory_order_acquire))
				(void)InvokeCommand(CommandType::shutdown);
			render_worker.request_stop();
			SetEvent(command_event.Get());
			render_worker.join();
		}
	}

	void StopWorkerAfterFailedInitialization() noexcept
	{
		if (!render_worker.joinable())
			return;
		render_worker.request_stop();
		SetEvent(command_event.Get());
		render_worker.join();
	}

	void PublishEffectSnapshotLocked(const bool force_reset) noexcept
	{
		const bool was_enabled = processing_enabled.load(std::memory_order_acquire);
		published_master_volume = master_volume;
		const auto publication = equalizer_settings.PreparePublication(
			static_cast<std::uint32_t>(output_format.sample_rate),
			force_reset,
			was_enabled,
			std::abs(published_master_volume - 1.0f) > 1.0e-6f);
		equalizer_snapshot = publication.snapshot;
		processing_enabled.store(
			publication.processing_enabled, std::memory_order_release);
	}

	[[nodiscard]] HRESULT ActivateClient()
	{
		IAudioClient* raw_client = device->ActivateAudioClient();
		if (!raw_client)
			return E_NOINTERFACE;
		audio_client.Attach(raw_client);
		return S_OK;
	}

	[[nodiscard]] REFERENCE_TIME SelectInitialPeriod() const noexcept
	{
		auto period = static_cast<REFERENCE_TIME>(
			device->GetDefaultDevicePeriod100Nanoseconds());
		if (period <= 0)
			period = FallbackDevicePeriod;
		const auto minimum_period = static_cast<REFERENCE_TIME>(
			device->GetMinimumDevicePeriod100Nanoseconds());
		return (std::max)(period, minimum_period);
	}

	[[nodiscard]] HRESULT InitializeAudioClient()
	{
		HRESULT result = ActivateClient();
		if (FAILED(result))
			return result;

		REFERENCE_TIME period = SelectInitialPeriod();
		result = audio_client->Initialize(
			AUDCLNT_SHAREMODE_EXCLUSIVE,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			period,
			period,
			&wave_format.Format,
			nullptr);
		if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			UINT32 aligned_frames = 0;
			const HRESULT size_result = audio_client->GetBufferSize(&aligned_frames);
			if (FAILED(size_result) || aligned_frames == 0)
				return FAILED(size_result) ? size_result : E_FAIL;

			audio_client.Reset();
			result = ActivateClient();
			if (FAILED(result))
				return result;
			period = static_cast<REFERENCE_TIME>(std::llround(
				static_cast<double>(WasapiReferenceTimeUnitsPerSecond) * aligned_frames /
				static_cast<double>(output_format.sample_rate)));
			result = audio_client->Initialize(
				AUDCLNT_SHAREMODE_EXCLUSIVE,
				AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				period,
				period,
				&wave_format.Format,
				nullptr);
		}
		if (FAILED(result))
			return result;

		result = audio_client->SetEventHandle(render_event.Get());
		if (FAILED(result))
			return result;

		UINT32 frame_count = 0;
		result = audio_client->GetBufferSize(&frame_count);
		if (FAILED(result) || frame_count == 0)
			return FAILED(result) ? result : E_FAIL;
		buffer_frame_count = frame_count;
		const auto buffer_duration_milliseconds =
			(static_cast<std::uint64_t>(buffer_frame_count) * 1'000u +
				static_cast<std::uint64_t>(output_format.sample_rate) - 1u) /
			static_cast<std::uint64_t>(output_format.sample_rate);
		render_wait_timeout_milliseconds = static_cast<DWORD>((std::clamp)(
			buffer_duration_milliseconds * 4u,
			std::uint64_t{20},
			std::uint64_t{250}));

		result = audio_client->GetService(
			__uuidof(IAudioRenderClient),
			reinterpret_cast<void**>(render_client.ReleaseAndGetAddressOf()));
		if (FAILED(result) || !render_client)
			return FAILED(result) ? result : E_NOINTERFACE;

		result = audio_client->GetService(
			__uuidof(IAudioClock),
			reinterpret_cast<void**>(audio_clock.ReleaseAndGetAddressOf()));
		if (FAILED(result) || !audio_clock)
			return FAILED(result) ? result : E_NOINTERFACE;
		result = audio_clock->GetFrequency(&clock_frequency);
		if (FAILED(result) || clock_frequency == 0)
			return FAILED(result) ? result : E_FAIL;
		NATIVE_TRACE(
			"info: WASAPI exclusive stream initialized: rate=%d, channels=%u, "
			"buffer_frames=%u, period_100ns=%lld, watchdog_ms=%lu, "
			"clock_frequency=%llu\n",
			output_format.sample_rate,
			output_format.channel_count,
			buffer_frame_count,
			static_cast<long long>(period),
			static_cast<unsigned long>(render_wait_timeout_milliseconds),
			static_cast<unsigned long long>(clock_frequency));

		const auto limiter = AudioDsp::MakeSinkLimiterConfig();
		if (!equalizer_dsp.Prepare(
			static_cast<std::uint32_t>(output_format.sample_rate),
			output_format.channel_count,
			buffer_frame_count,
			limiter))
		{
			return E_FAIL;
		}
		if (!staged_equalizer_dsp.Prepare(
			static_cast<std::uint32_t>(output_format.sample_rate),
			output_format.channel_count,
			buffer_frame_count,
			limiter))
		{
			return E_FAIL;
		}

		const auto byte_capacity = static_cast<std::size_t>(buffer_frame_count) *
			block_align;
		const auto sample_capacity = static_cast<std::size_t>(buffer_frame_count) *
			output_format.channel_count;
		byte_scratch.resize(byte_capacity);
		render_scratch.resize(byte_capacity);
		float_input.resize(sample_capacity);
		float_output.resize(sample_capacity);
		return S_OK;
	}

	void PublishInitializationResult(const HRESULT result) noexcept
	{
		{
			std::lock_guard lock(initialization_mutex);
			initialization_result = result;
			initialization_complete = true;
		}
		initialization_cv.notify_all();
	}

	void ReleaseAudioClientResources() noexcept
	{
		started.store(false, std::memory_order_release);
		initialized.store(false, std::memory_order_release);
		clock_health_monitor.Reset();
		consecutive_render_timeouts = 0;
		consecutive_buffer_errors = 0;
		consecutive_media_underruns = 0;
		has_render_service_time = false;
		audio_clock.Reset();
		render_client.Reset();
		audio_client.Reset();
		worker_running.store(false, std::memory_order_release);
		command_cv.notify_all();
	}

	void RenderThread(const std::stop_token stop_token) noexcept
	{
		ComApartment com;
		const HRESULT com_result = com.Result();
		if (FAILED(com_result))
		{
			PublishInitializationResult(com_result);
			worker_running.store(false, std::memory_order_release);
			command_cv.notify_all();
			return;
		}

		std::unique_ptr<IAudioThreadScheduleHelper> thread_schedule;
		try
		{
			thread_schedule = CreateDefaultAudioThreadScheduleHelper(
				ProAudioMmcssTaskName,
				MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_CRITICAL,
				"WASAPI render");
			if (!thread_schedule)
			{
				NATIVE_TRACE(
					"warn: audio thread scheduler is unavailable for WASAPI render\n");
			}
		}
		catch (...)
		{
			// Scheduling is an optimization. It must not make a valid endpoint
			// fail initialization from this noexcept worker.
			NATIVE_TRACE(
				"warn: audio thread scheduler failed for WASAPI render\n");
		}

		HRESULT result = E_FAIL;
		try
		{
			result = InitializeAudioClient();
		}
		catch (...)
		{
			result = E_FAIL;
		}
		if (FAILED(result))
		{
			PublishInitializationResult(result);
			ReleaseAudioClientResources();
			return;
		}

		initialized.store(true, std::memory_order_release);
		worker_running.store(true, std::memory_order_release);
		PublishInitializationResult(S_OK);

		const std::array<HANDLE, 2> wait_handles{
			command_event.Get(), render_event.Get()
		};
		bool keep_running = true;
		while (keep_running && !stop_token.stop_requested())
		{
			const DWORD wait_timeout = started.load(std::memory_order_acquire)
				? render_wait_timeout_milliseconds
				: INFINITE;
			const DWORD wait_result = WaitForMultipleObjects(
				static_cast<DWORD>(wait_handles.size()),
				wait_handles.data(),
				FALSE,
				wait_timeout);
			if (wait_result == WAIT_OBJECT_0)
			{
				keep_running = HandleCommands();
			}
			else if (wait_result == WAIT_OBJECT_0 + 1)
			{
				if (started.load(std::memory_order_acquire))
				{
					result = RenderEventBuffer();
					if (result == AUDCLNT_E_BUFFER_ERROR)
					{
						++consecutive_buffer_errors;
						NATIVE_TRACE(
							"warn: WASAPI render buffer unavailable (%u consecutive)\n",
							consecutive_buffer_errors);
						if (consecutive_buffer_errors >= 2)
							result = RecoverClient("repeated render buffer errors");
						else
							result = S_OK;
					}
					else if (SUCCEEDED(result))
					{
						consecutive_buffer_errors = 0;
					}
					if (FAILED(result))
						HandleRenderFailure(result);
				}
			}
			else if (wait_result == WAIT_TIMEOUT)
			{
				if (started.load(std::memory_order_acquire))
				{
					result = HandleRenderWatchdogTimeout();
					if (FAILED(result))
						HandleRenderFailure(result);
				}
			}
			else
			{
				HandleRenderFailure(HRESULT_FROM_WIN32(GetLastError()));
				break;
			}
		}

		if (audio_client)
		{
			(void)audio_client->Stop();
			(void)audio_client->Reset();
		}
		ReleaseAudioClientResources();
	}

	[[nodiscard]] HRESULT InvokeCommand(const CommandType type) noexcept
	{
		std::unique_lock invoke_lock(command_invoke_mutex);
		if (!worker_running.load(std::memory_order_acquire))
			return E_FAIL;

		std::uint64_t serial = 0;
		{
			std::lock_guard command_lock(command_mutex);
			serial = ++next_command_serial;
			commands.push_back({type, serial});
		}
		if (!SetEvent(command_event.Get()))
			return HRESULT_FROM_WIN32(GetLastError());

		std::unique_lock command_lock(command_mutex);
		const bool completed = command_cv.wait_for(
			command_lock,
			CommandWaitTimeout,
			[this, serial]
			{
				return completed_command_serial >= serial ||
					!worker_running.load(std::memory_order_acquire);
			});
		if (!completed || completed_command_serial < serial)
			return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
		return completed_command_result;
	}

	[[nodiscard]] bool HandleCommands() noexcept
	{
		for (;;)
		{
			Command command{};
			{
				std::lock_guard lock(command_mutex);
				if (commands.empty())
					return true;
				command = commands.front();
				commands.pop_front();
			}

			HRESULT result = S_OK;
			bool keep_running = true;
			switch (command.type)
			{
			case CommandType::start:
				result = StartClient();
				break;
			case CommandType::stop:
				result = StopClient();
				break;
			case CommandType::reset:
				result = ResetClient();
				break;
			case CommandType::shutdown:
				result = ResetClient();
				keep_running = false;
				break;
			}

			{
				std::lock_guard lock(command_mutex);
				completed_command_result = result;
				completed_command_serial = command.serial;
			}
			command_cv.notify_all();
			if (!keep_running)
				return false;
		}
	}

	[[nodiscard]] HRESULT StartClient() noexcept
	{
		if (!audio_client || !render_client || !audio_clock)
			return E_POINTER;
		if (started.load(std::memory_order_acquire))
			return S_OK;

		HRESULT result = S_OK;
		if (needs_initial_prefill)
		{
			// Exclusive event-driven rendering requires one whole-buffer prefill
			// before the first Start after Initialize/Reset. Stop/Start resumes the
			// retained endpoint buffers without submitting a duplicate packet.
			result = RenderWholeBuffer();
			if (FAILED(result))
				return result;
			needs_initial_prefill = false;
		}
		UINT64 position = 0;
		UINT64 qpc_position_100ns = 0;
		result = audio_clock->GetPosition(&position, &qpc_position_100ns);
		if (result == S_FALSE)
			result = audio_clock->GetPosition(&position, &qpc_position_100ns);
		if (FAILED(result))
			return result;
		if (!clock_origin_valid)
		{
			clock_origin_position = position;
			clock_origin_valid = true;
		}
		clock_health_monitor.Reset();
		if (SUCCEEDED(result))
		{
			(void)clock_health_monitor.Observe(
				position, clock_frequency, qpc_position_100ns);
		}
		result = audio_client->Start();
		if (SUCCEEDED(result))
		{
			last_render_service_time = std::chrono::steady_clock::now();
			has_render_service_time = true;
			started.store(true, std::memory_order_release);
		}
		return result;
	}

	[[nodiscard]] HRESULT StopClient() noexcept
	{
		if (!audio_client)
			return E_POINTER;
		if (!started.exchange(false, std::memory_order_acq_rel))
			return S_OK;
		clock_health_monitor.Reset();
		has_render_service_time = false;
		consecutive_render_timeouts = 0;
		consecutive_buffer_errors = 0;
		return audio_client->Stop();
	}

	[[nodiscard]] HRESULT ResetClient() noexcept
	{
		if (!audio_client)
			return E_POINTER;
		HRESULT result = S_OK;
		if (started.exchange(false, std::memory_order_acq_rel))
			result = audio_client->Stop();
		const HRESULT reset_result = audio_client->Reset();
		if (SUCCEEDED(result))
			result = reset_result;
		if (SUCCEEDED(reset_result))
		{
			clock_origin_position = 0;
			clock_origin_valid = false;
			clock_health_monitor.Reset();
			consecutive_render_timeouts = 0;
			consecutive_buffer_errors = 0;
			consecutive_media_underruns = 0;
			has_render_service_time = false;
			needs_initial_prefill = true;
			if (!ResetEvent(render_event.Get()) && SUCCEEDED(result))
				result = HRESULT_FROM_WIN32(GetLastError());
		}
		return result;
	}

	void HandleRenderFailure(const HRESULT result) noexcept
	{
		NATIVE_TRACE(
			"err: WASAPI exclusive render failed, HRESULT=0x%08X\n",
			HResultCode(result));
		if (audio_client)
			(void)audio_client->Stop();
		started.store(false, std::memory_order_release);
		MarkStreamError(result);
	}

	void MarkStreamError(const HRESULT result) noexcept
	{
		{
			std::lock_guard lock(state_mutex);
			error_code = HResultCode(result);
			phase = StreamPhase::error;
		}
		stream_end_cv.notify_all();
	}

	[[nodiscard]] HRESULT ReadPresentedDeviceFrames(
		std::uint64_t& frame_count,
		std::uint64_t& device_position,
		std::uint64_t& qpc_position_100ns) const noexcept
	{
		frame_count = 0;
		device_position = 0;
		qpc_position_100ns = 0;
		if (!audio_clock || !clock_origin_valid || clock_frequency == 0)
			return E_UNEXPECTED;

		UINT64 position = 0;
		UINT64 qpc_position = 0;
		HRESULT result = audio_clock->GetPosition(
			&position, &qpc_position);
		if (result == S_FALSE)
		{
			// Microsoft explicitly permits one bounded retry for an inaccurate
			// exclusive-mode clock read. Never spin here under sustained load.
			result = audio_clock->GetPosition(&position, &qpc_position);
		}
		if (FAILED(result))
			return result;
		device_position = position;
		qpc_position_100ns = qpc_position;
		if (position <= clock_origin_position)
			return result;

		const auto elapsed_ticks = position - clock_origin_position;
		const auto sample_rate = static_cast<std::uint64_t>(
			output_format.sample_rate);
		const auto whole_seconds = elapsed_ticks / clock_frequency;
		const auto remainder = elapsed_ticks % clock_frequency;
		if (whole_seconds >
			(std::numeric_limits<std::uint64_t>::max)() / sample_rate)
		{
			frame_count = (std::numeric_limits<std::uint64_t>::max)();
			return S_OK;
		}
		frame_count = whole_seconds * sample_rate;
		if (remainder <=
			(std::numeric_limits<std::uint64_t>::max)() / sample_rate)
		{
			frame_count += remainder * sample_rate / clock_frequency;
		}
		else
		{
			frame_count += static_cast<std::uint64_t>(
				static_cast<long double>(remainder) * sample_rate /
				clock_frequency);
		}
		return result;
	}

	void UpdatePlaybackClockLocked(
		const std::uint64_t presented_device_frames) noexcept
	{
		consumed_device_frames = (std::max)(
			consumed_device_frames,
			(std::min)(presented_device_frames, total_device_frames_written));
		const auto remaining_frames = total_device_frames_written -
			consumed_device_frames;
		last_padding = SaturateToUint32(remaining_frames);

		while (!clock_runs.empty())
		{
			const auto& run = clock_runs.front();
			const auto run_end = run.device_start + run.device_frames;
			if (consumed_device_frames >= run_end)
			{
				if (run.contains_media)
				{
					presented_media_frames = (std::max)(
						presented_media_frames,
						run.media_start + run.device_frames);
				}
				clock_runs.pop_front();
				continue;
			}
			if (run.contains_media && consumed_device_frames > run.device_start)
			{
				presented_media_frames = std::max(
					presented_media_frames,
					run.media_start + consumed_device_frames - run.device_start);
			}
			break;
		}
		presented_media_frames = std::min(
			presented_media_frames, submitted_media_frames);
	}

	[[nodiscard]] bool CompleteIfDrainedLocked() noexcept
	{
		if (!drain_output_finished ||
			consumed_device_frames < drain_target_device_frames ||
			(phase != StreamPhase::draining && phase != StreamPhase::eos_queued))
		{
			return false;
		}
		presented_media_frames = submitted_media_frames;
		completed_generation = generation;
		phase = StreamPhase::ended;
		return true;
	}

	void AppendClockRunsLocked(
		const std::uint32_t device_frames,
		const std::uint32_t media_frames)
	{
		if (media_frames != 0)
		{
			clock_runs.push_back({
				.device_start = total_device_frames_written,
				.device_frames = media_frames,
				.media_start = media_frames_written,
				.contains_media = true
			});
			media_frames_written += media_frames;
			total_device_frames_written += media_frames;
		}
		if (device_frames > media_frames)
		{
			clock_runs.push_back({
				.device_start = total_device_frames_written,
				.device_frames = device_frames - media_frames,
				.media_start = media_frames_written,
				.contains_media = false
			});
			total_device_frames_written += device_frames - media_frames;
		}
	}

	[[nodiscard]] std::uint32_t CopyMediaLocked(
		const std::uint32_t capacity_frames)
	{
		while (!queue.empty() &&
			(queue.front().generation != generation ||
				queue.front().consumed_frames >= queue.front().frame_count))
		{
			queue.pop_front();
		}

		std::uint32_t copied_frames = 0;
		for (const auto& block : queue)
		{
			if (copied_frames >= capacity_frames)
				break;
			if (block.generation != generation ||
				block.consumed_frames >= block.frame_count)
				continue;
			const auto remaining = block.frame_count - block.consumed_frames;
			const auto count = (std::min)(remaining, capacity_frames - copied_frames);
			const auto source_offset = static_cast<std::size_t>(
				block.consumed_frames) * block_align;
			const auto destination_offset = static_cast<std::size_t>(
				copied_frames) * block_align;
			const auto byte_count = static_cast<std::size_t>(count) * block_align;
			std::memcpy(
				byte_scratch.data() + destination_offset,
				block.bytes.data() + source_offset,
				byte_count);
			copied_frames += count;
		}
		return copied_frames;
	}

	void CommitMediaLocked(std::uint32_t frame_count) noexcept
	{
		while (frame_count != 0 && !queue.empty())
		{
			auto& block = queue.front();
			if (block.generation != generation ||
				block.consumed_frames >= block.frame_count)
			{
				queue.pop_front();
				continue;
			}
			const auto remaining = block.frame_count - block.consumed_frames;
			const auto count = (std::min)(remaining, frame_count);
			block.consumed_frames += count;
			frame_count -= count;
			if (block.consumed_frames == block.frame_count)
				queue.pop_front();
		}
	}

	[[nodiscard]] bool HasMediaAfterFramesLocked(
		std::uint32_t frame_count) const noexcept
	{
		for (const auto& block : queue)
		{
			if (block.generation != generation ||
				block.consumed_frames >= block.frame_count)
			{
				continue;
			}
			const auto remaining = block.frame_count - block.consumed_frames;
			if (frame_count < remaining)
				return true;
			frame_count -= remaining;
		}
		return false;
	}

	[[nodiscard]] HRESULT RenderEventBuffer() noexcept
	{
		const auto service_time = std::chrono::steady_clock::now();
		const bool render_service_late = has_render_service_time &&
			service_time - last_render_service_time >
				std::chrono::milliseconds(render_wait_timeout_milliseconds);
		last_render_service_time = service_time;
		has_render_service_time = true;

		std::uint64_t presented_device_frames = 0;
		std::uint64_t device_position = 0;
		std::uint64_t qpc_position_100ns = 0;
		HRESULT result = ReadPresentedDeviceFrames(
			presented_device_frames,
			device_position,
			qpc_position_100ns);
		if (FAILED(result))
			return result;
		consecutive_render_timeouts = 0;
		const bool clock_rate_unhealthy = SUCCEEDED(result) &&
			clock_health_monitor.Observe(
				device_position, clock_frequency, qpc_position_100ns);

		bool completed = false;
		{
			std::lock_guard state_lock(state_mutex);
			// An exclusive event-driven stream ping-pongs two endpoint buffers.
			// The hardware clock, rather than padding or event count, determines
			// which media frames have actually reached the endpoint.
			UpdatePlaybackClockLocked(presented_device_frames);
			completed = CompleteIfDrainedLocked();
		}
		if (completed)
		{
			result = StopClient();
			stream_end_cv.notify_all();
			return result;
		}
		if (render_service_late || clock_rate_unhealthy)
		{
			if (render_service_late)
			{
				NATIVE_TRACE(
					"warn: WASAPI render service missed its %lu ms deadline\n",
					static_cast<unsigned long>(render_wait_timeout_milliseconds));
			}
			return RecoverClient(render_service_late
				? "late render service"
				: "device clock rate drift");
		}
		return RenderWholeBuffer();
	}

	[[nodiscard]] HRESULT HandleRenderWatchdogTimeout() noexcept
	{
		std::uint64_t presented_device_frames = 0;
		std::uint64_t device_position = 0;
		std::uint64_t qpc_position_100ns = 0;
		HRESULT result = ReadPresentedDeviceFrames(
			presented_device_frames,
			device_position,
			qpc_position_100ns);
		if (FAILED(result))
			return result;
		const bool clock_rate_unhealthy = SUCCEEDED(result) &&
			clock_health_monitor.Observe(
				device_position, clock_frequency, qpc_position_100ns);

		bool completed = false;
		{
			std::lock_guard state_lock(state_mutex);
			UpdatePlaybackClockLocked(presented_device_frames);
			completed = CompleteIfDrainedLocked();
		}
		if (completed)
		{
			result = StopClient();
			stream_end_cv.notify_all();
			return result;
		}

		++consecutive_render_timeouts;
		NATIVE_TRACE(
			"warn: WASAPI render event watchdog timed out (%u consecutive)\n",
			consecutive_render_timeouts);
		if (clock_rate_unhealthy || consecutive_render_timeouts >= 2)
		{
			return RecoverClient(clock_rate_unhealthy
				? "device clock rate drift during render timeout"
				: "render event timeout");
		}
		return S_OK;
	}

	[[nodiscard]] HRESULT RenderWholeBuffer() noexcept
	{
		DWORD release_flags = 0;
		bool output_finished_after_buffer = false;
		std::uint32_t media_frames = 0;
		AudioStreamGeneration rendered_generation = 0;
		bool draining_with_empty_queue = false;
		bool media_underrun = false;
		{
			std::lock_guard state_lock(state_mutex);
			rendered_generation = generation;
			media_frames = CopyMediaLocked(buffer_frame_count);
			const bool draining = phase == StreamPhase::draining ||
				phase == StreamPhase::eos_queued;
			if (!draining && media_frames < buffer_frame_count)
			{
				media_frames = 0;
				media_underrun = true;
				if (consecutive_media_underruns !=
					(std::numeric_limits<std::uint32_t>::max)())
				{
					++consecutive_media_underruns;
				}
			}
			else
			{
				consecutive_media_underruns = 0;
			}
			draining_with_empty_queue =
				draining &&
				!HasMediaAfterFramesLocked(media_frames);
		}
		if (media_underrun &&
			(consecutive_media_underruns == 1 ||
				(consecutive_media_underruns &
					(consecutive_media_underruns - 1)) == 0))
		{
			NATIVE_TRACE(
				"warn: WASAPI media underrun (%u consecutive buffers)\n",
				consecutive_media_underruns);
		}

		const bool use_processing =
			processing_enabled.load(std::memory_order_acquire);
		AudioDsp::EqualizerDspSnapshot snapshot{};
		float render_volume = 1.0f;
		if (use_processing)
		{
			std::lock_guard effect_lock(effect_mutex);
			snapshot = equalizer_snapshot;
			render_volume = published_master_volume;
		}

		if (!use_processing)
		{
			const auto media_bytes = static_cast<std::size_t>(media_frames) *
				block_align;
			if (media_bytes != 0)
			{
				std::memcpy(
					render_scratch.data(), byte_scratch.data(), media_bytes);
			}
			const auto remaining_bytes =
				static_cast<std::size_t>(buffer_frame_count - media_frames) *
				block_align;
			if (remaining_bytes != 0)
			{
				std::memset(
					render_scratch.data() + media_bytes, 0, remaining_bytes);
			}
			if (media_frames == 0)
				release_flags = AUDCLNT_BUFFERFLAGS_SILENT;
		}
		else
		{
			try
			{
				// Work on a pre-sized copy. The committed DSP state advances only
				// after WASAPI accepts the corresponding endpoint packet.
				staged_equalizer_dsp = equalizer_dsp;
			}
			catch (...)
			{
				return E_OUTOFMEMORY;
			}
			if (media_frames != 0)
			{
				const auto media_sample_count =
					static_cast<std::size_t>(media_frames) *
					output_format.channel_count;
				const auto media_byte_count =
					static_cast<std::size_t>(media_frames) * block_align;
				if (!AudioDsp::DecodePcmSamples(
					std::span<const std::uint8_t>(
						byte_scratch.data(), media_byte_count),
					std::span<float>(
						float_input.data(), media_sample_count),
					output_format.bit_depth,
					output_format.sample_format))
				{
					return E_FAIL;
				}
				if (!staged_equalizer_dsp.Process(
					snapshot,
					float_input.data(),
					float_output.data(),
					media_frames,
					false))
				{
					return E_FAIL;
				}
			}

			const auto silent_frames = buffer_frame_count - media_frames;
			if (silent_frames != 0)
			{
				const auto sample_offset = static_cast<std::size_t>(media_frames) *
					output_format.channel_count;
				(void)staged_equalizer_dsp.Process(
					snapshot,
					nullptr,
					float_output.data() + sample_offset,
					silent_frames,
					true);
			}

			if (std::abs(render_volume - 1.0f) > 1.0e-6f)
			{
				const auto sample_count =
					static_cast<std::size_t>(buffer_frame_count) *
					output_format.channel_count;
				for (std::size_t index = 0; index < sample_count; ++index)
					float_output[index] *= render_volume;
			}
			const auto output_sample_count =
				static_cast<std::size_t>(buffer_frame_count) *
				output_format.channel_count;
			const auto output_byte_count =
				static_cast<std::size_t>(buffer_frame_count) * block_align;
			if (!AudioDsp::EncodePcmSamples(
				std::span<const float>(
					float_output.data(), output_sample_count),
				std::span<std::uint8_t>(
					render_scratch.data(), output_byte_count),
				output_format.bit_depth,
				output_format.sample_format))
			{
				return E_FAIL;
			}
		}
		const bool tail_remaining =
			use_processing && staged_equalizer_dsp.HasTail();
		output_finished_after_buffer =
			draining_with_empty_queue && !tail_remaining;

		// Do every allocation, lock acquisition, PCM conversion and DSP pass before
		// acquiring the endpoint packet. WASAPI expects GetBuffer/ReleaseBuffer to
		// complete within the same processing period.
		BYTE* render_bytes = nullptr;
		HRESULT result = render_client->GetBuffer(
			buffer_frame_count, &render_bytes);
		if (FAILED(result) || !render_bytes)
			return FAILED(result) ? result : E_POINTER;
		if (release_flags == 0)
		{
			const auto output_byte_count =
				static_cast<std::size_t>(buffer_frame_count) * block_align;
			std::memcpy(render_bytes, render_scratch.data(), output_byte_count);
		}

		result = render_client->ReleaseBuffer(buffer_frame_count, release_flags);
		if (FAILED(result))
			return result;
		if (use_processing)
			std::swap(equalizer_dsp, staged_equalizer_dsp);

		{
			std::lock_guard state_lock(state_mutex);
			if (generation != rendered_generation ||
				(phase != StreamPhase::open &&
					phase != StreamPhase::draining &&
					phase != StreamPhase::eos_queued))
			{
				return S_OK;
			}
			CommitMediaLocked(media_frames);
			AppendClockRunsLocked(buffer_frame_count, media_frames);
			const auto outstanding_frames = total_device_frames_written -
				consumed_device_frames;
			last_padding = SaturateToUint32(outstanding_frames);
			if (output_finished_after_buffer)
			{
				if (!drain_output_finished)
				{
					drain_output_finished = true;
					drain_target_device_frames = total_device_frames_written;
				}
				phase = StreamPhase::eos_queued;
			}
		}
		return S_OK;
	}

	[[nodiscard]] HRESULT RecoverClient(const char* reason) noexcept
	{
		const HRESULT reset_result = ResetClient();
		if (FAILED(reset_result))
			return reset_result;

		std::uint64_t skipped_media_frames = 0;
		{
			std::lock_guard state_lock(state_mutex);
			const auto written_media_frames = (std::min)(
				media_frames_written, submitted_media_frames);
			if (written_media_frames > presented_media_frames)
				skipped_media_frames = written_media_frames - presented_media_frames;
			presented_media_frames = (std::max)(
				presented_media_frames, written_media_frames);
			clock_runs.clear();
			last_padding = 0;
			total_device_frames_written = 0;
			consumed_device_frames = 0;
			drain_target_device_frames = 0;
			drain_output_finished = false;
		}
		{
			std::lock_guard effect_lock(effect_mutex);
			PublishEffectSnapshotLocked(true);
		}

		++recovery_count;
		NATIVE_TRACE(
			"warn: WASAPI stream recovery #%llu: %s, skipped_media_frames=%llu\n",
			static_cast<unsigned long long>(recovery_count),
			reason ? reason : "unknown",
			static_cast<unsigned long long>(skipped_media_frames));
		return StartClient();
	}

	void ClearGenerationStateLocked() noexcept
	{
		queue.clear();
		clock_runs.clear();
		completed_generation = 0;
		error_code = 0;
		last_padding = 0;
		submitted_media_frames = 0;
		media_frames_written = 0;
		presented_media_frames = 0;
		total_device_frames_written = 0;
		consumed_device_frames = 0;
		drain_target_device_frames = 0;
		drain_output_finished = false;
		consecutive_media_underruns = 0;
	}

	[[nodiscard]] bool AbortStreamUnderLifecycleLock() noexcept
	{
		{
			std::lock_guard state_lock(state_mutex);
			phase = StreamPhase::aborted;
			queue.clear();
			drain_target_device_frames = 0;
			drain_output_finished = false;
		}
		const HRESULT result = InvokeCommand(CommandType::reset);
		{
			std::lock_guard state_lock(state_mutex);
			clock_runs.clear();
			last_padding = 0;
			media_frames_written = 0;
			presented_media_frames = 0;
			total_device_frames_written = 0;
			consumed_device_frames = 0;
			if (FAILED(result))
			{
				error_code = HResultCode(result);
				phase = StreamPhase::error;
			}
			else
			{
				phase = StreamPhase::aborted;
			}
		}
		stream_end_cv.notify_all();
		return SUCCEEDED(result);
	}

	[[nodiscard]] bool AbortStreamInternal() noexcept
	{
		std::lock_guard lifecycle_lock(lifecycle_mutex);
		return AbortStreamUnderLifecycleLock();
	}
};

MusicPlayerLibrary::WasapiExclusiveSink::WasapiExclusiveSink(
	const AudioOutputFormat& requested,
	std::shared_ptr<WasapiExclusiveOutputDevice> device) :
	impl_(std::make_unique<Impl>(requested, std::move(device)))
{
}

MusicPlayerLibrary::WasapiExclusiveSink::~WasapiExclusiveSink() = default;

const MusicPlayerLibrary::AudioOutputFormat&
MusicPlayerLibrary::WasapiExclusiveSink::GetOutputFormat() const noexcept
{
	return impl_->output_format;
}

const MusicPlayerLibrary::AudioOutputFormat&
MusicPlayerLibrary::WasapiExclusiveSink::GetDeviceFormat() const noexcept
{
	return impl_->device->GetOutputFormat();
}

bool MusicPlayerLibrary::WasapiExclusiveSink::IsInitialized() const noexcept
{
	return impl_ && impl_->initialized.load(std::memory_order_acquire);
}

bool MusicPlayerLibrary::WasapiExclusiveSink::IsLimiterEnabled() const noexcept
{
	return impl_ && impl_->processing_enabled.load(std::memory_order_acquire);
}

std::uint32_t
MusicPlayerLibrary::WasapiExclusiveSink::GetPreferredSubmitFrameCount()
	const noexcept
{
	return impl_ ? impl_->buffer_frame_count : 0;
}

MusicPlayerLibrary::AudioStreamGeneration
MusicPlayerLibrary::WasapiExclusiveSink::BeginStream()
{
	if (!impl_)
		throw std::runtime_error("WASAPI exclusive stream is unavailable");
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	if (!impl_->AbortStreamUnderLifecycleLock())
		throw std::runtime_error("WASAPI exclusive stream reset failed");

	std::lock_guard state_lock(impl_->state_mutex);
	impl_->ClearGenerationStateLocked();
	++impl_->generation;
	{
		std::lock_guard effect_lock(impl_->effect_mutex);
		impl_->PublishEffectSnapshotLocked(true);
	}
	impl_->phase = Impl::StreamPhase::open;
	return impl_->generation;
}

bool MusicPlayerLibrary::WasapiExclusiveSink::Submit(
	const NormalizedPcmBlock& block)
{
	if (!impl_ || block.bytes.empty() || block.frame_count == 0)
		return false;
	if (block.frame_count >
		(std::numeric_limits<std::size_t>::max)() / impl_->block_align)
	{
		return false;
	}
	const auto expected_bytes = static_cast<std::size_t>(block.frame_count) *
		impl_->block_align;
	if (block.bytes.size() != expected_bytes)
		return false;

	try
	{
		Impl::QueuedBlock queued{
			.bytes = std::vector<std::uint8_t>(
				block.bytes.begin(), block.bytes.end()),
			.frame_count = block.frame_count,
			.consumed_frames = 0,
			.generation = block.generation
		};
		std::lock_guard state_lock(impl_->state_mutex);
		if (impl_->phase != Impl::StreamPhase::open ||
			block.generation != impl_->generation)
		{
			return false;
		}
		impl_->queue.push_back(std::move(queued));
		impl_->submitted_media_frames += block.frame_count;
		if (block.end_of_stream)
			impl_->phase = Impl::StreamPhase::draining;
	}
	catch (...)
	{
		return false;
	}
	return true;
}

bool MusicPlayerLibrary::WasapiExclusiveSink::EndStream() noexcept
{
	if (!impl_)
		return false;
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	{
		std::lock_guard state_lock(impl_->state_mutex);
		if (impl_->phase != Impl::StreamPhase::open || impl_->generation == 0)
			return false;
		impl_->phase = Impl::StreamPhase::draining;
	}
	return true;
}

bool MusicPlayerLibrary::WasapiExclusiveSink::Start() noexcept
{
	if (!impl_)
		return false;
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	{
		std::lock_guard state_lock(impl_->state_mutex);
		if (impl_->generation == 0 ||
			(impl_->phase != Impl::StreamPhase::open &&
				impl_->phase != Impl::StreamPhase::draining &&
				impl_->phase != Impl::StreamPhase::eos_queued))
		{
			return false;
		}
	}
	const HRESULT result = impl_->InvokeCommand(Impl::CommandType::start);
	if (FAILED(result))
		impl_->MarkStreamError(result);
	return SUCCEEDED(result);
}

void MusicPlayerLibrary::WasapiExclusiveSink::Stop() noexcept
{
	if (!impl_)
		return;
	std::lock_guard lifecycle_lock(impl_->lifecycle_mutex);
	const HRESULT result = impl_->InvokeCommand(Impl::CommandType::stop);
	if (FAILED(result))
		impl_->MarkStreamError(result);
}

void MusicPlayerLibrary::WasapiExclusiveSink::AbortStream() noexcept
{
	if (impl_)
		(void)impl_->AbortStreamInternal();
}

MusicPlayerLibrary::AudioSinkState
MusicPlayerLibrary::WasapiExclusiveSink::GetState() const noexcept
{
	AudioSinkState result{};
	if (!impl_)
		return result;

	std::lock_guard state_lock(impl_->state_mutex);
	result.generation = impl_->generation;
	result.error_code = impl_->error_code;
	if (impl_->phase == Impl::StreamPhase::idle ||
		impl_->phase == Impl::StreamPhase::aborted)
	{
		return result;
	}
	result.stream_ended = impl_->generation != 0 &&
		impl_->completed_generation == impl_->generation;
	result.samples_played = impl_->presented_media_frames;
	result.media_frames_presented = impl_->presented_media_frames;
	if (result.stream_ended)
		return result;

	const auto queued_frames = impl_->submitted_media_frames >=
		impl_->presented_media_frames
		? impl_->submitted_media_frames - impl_->presented_media_frames
		: 0;
	result.queued_frames = queued_frames;
	const bool has_device_media = std::any_of(
		impl_->clock_runs.begin(), impl_->clock_runs.end(),
		[](const Impl::ClockRun& run) noexcept { return run.contains_media; });
	const auto buffer_count = static_cast<std::uint64_t>(impl_->queue.size()) +
		(has_device_media ? 1u : 0u);
	result.buffers_queued = SaturateToUint32(buffer_count);
	const auto latency = static_cast<std::uint64_t>(impl_->last_padding) +
		impl_->equalizer_dsp.GetLimiterDelayFrames();
	result.presentation_latency_frames = SaturateToUint32(latency);
	return result;
}

bool MusicPlayerLibrary::WasapiExclusiveSink::WaitForStreamEnd(
	const std::chrono::milliseconds timeout)
{
	if (!impl_)
		return false;
	std::unique_lock state_lock(impl_->state_mutex);
	const auto expected_generation = impl_->generation;
	if (expected_generation == 0)
		return false;
	const auto predicate = [this, expected_generation]
		{
			return impl_->generation != expected_generation ||
				impl_->completed_generation == expected_generation ||
				impl_->phase == Impl::StreamPhase::error ||
				impl_->phase == Impl::StreamPhase::aborted;
		};
	if (timeout <= std::chrono::milliseconds::zero())
		return predicate();
	if (!impl_->stream_end_cv.wait_for(state_lock, timeout, predicate))
		return false;
	return impl_->generation == expected_generation;
}

void MusicPlayerLibrary::WasapiExclusiveSink::SetMasterVolume(
	float volume) noexcept
{
	if (!impl_)
		return;
	volume = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 1.0f;
	std::lock_guard state_lock(impl_->state_mutex);
	std::lock_guard effect_lock(impl_->effect_mutex);
	if (std::abs(impl_->master_volume - volume) <= 1.0e-6f)
		return;
	impl_->master_volume = volume;
	if (impl_->phase != Impl::StreamPhase::draining &&
		impl_->phase != Impl::StreamPhase::eos_queued)
	{
		impl_->PublishEffectSnapshotLocked(false);
	}
}

int MusicPlayerLibrary::WasapiExclusiveSink::GetEqualizerBand(
	const int index) const noexcept
{
	if (!impl_)
		return 0;
	std::lock_guard effect_lock(impl_->effect_mutex);
	return impl_->equalizer_settings.GetBand(index);
}

void MusicPlayerLibrary::WasapiExclusiveSink::SetEqualizerBand(
	const int index,
	int value) noexcept
{
	if (!impl_)
		return;
	std::lock_guard state_lock(impl_->state_mutex);
	std::lock_guard effect_lock(impl_->effect_mutex);
	if (!impl_->equalizer_settings.SetBand(index, value))
		return;
	if (impl_->phase == Impl::StreamPhase::draining ||
		impl_->phase == Impl::StreamPhase::eos_queued)
	{
		return;
	}
	impl_->PublishEffectSnapshotLocked(false);
}

std::shared_ptr<MusicPlayerLibrary::WasapiExclusiveOutputDevice>
MusicPlayerLibrary::WasapiExclusiveSink::GetDevice() const noexcept
{
	return impl_ ? impl_->device : nullptr;
}

#endif // defined(_WIN32)
