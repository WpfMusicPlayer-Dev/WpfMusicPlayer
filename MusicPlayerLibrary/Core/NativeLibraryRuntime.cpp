// SPDX-License-Identifier: MIT

#include "pch.h"

#include <mutex>
#include <stdexcept>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4251 4267 4273)
#endif
#include <ncnn/gpu.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "Audio/FFT/AudioPipelinePerformanceHelper.h"
#include "Audio/Pipeline/Device/AudioOutputDeviceLifecycle.h"
#include "Core/NativeLibraryRuntime.h"
#include "Core/NativeTraceRedirect.h"
#include "Lyric/LrcFileController.h"

namespace
{
	enum class NativeRuntimeState
	{
		NotInitialized,
		Initialized,
		ShuttingDown,
		Shutdown
	};

	std::mutex runtime_mutex;
	NativeRuntimeState runtime_state = NativeRuntimeState::NotInitialized;
	bool language_helper_initialized = false;

	void ShutdownLanguageHelper() noexcept
	{
		if (!language_helper_initialized)
			return;
		MusicPlayerLibrary::LrcLanguageHelper::ShutdownSingleton();
		language_helper_initialized = false;
	}

	void ShutdownOutputAndTrace() noexcept
	{
		MusicPlayerLibrary::ShutdownAudioOutputDevices();
		::NativeTraceRedirect::ShutdownNativeTraceRedirect();
	}

	void ShutdownInitializedResources(const bool announce_shutdown) noexcept
	{
		ShutdownLanguageHelper();
		ncnn::destroy_gpu_instance();
		if (announce_shutdown)
			NATIVE_TRACE("info: native library runtime shut down");
		ShutdownOutputAndTrace();
	}
}

void MusicPlayerLibrary::NativeLibraryRuntime::Initialize()
{
	std::lock_guard lock(runtime_mutex);
	if (runtime_state == NativeRuntimeState::Initialized)
		return;
	if (runtime_state != NativeRuntimeState::NotInitialized)
		throw std::logic_error("native library runtime cannot be initialized after shutdown");

	NATIVE_TRACE("info: native library runtime initializing");
	try
	{
		PrecomputeAudioPipelineBufferingProfile();
		LrcLanguageHelper::InitializeSingleton();
		language_helper_initialized = true;
		runtime_state = NativeRuntimeState::Initialized;
		NATIVE_TRACE("info: native library runtime initialized");
	}
	catch (...)
	{
		// 歌词解析器失败的时候，仍然需要清理底层的GPU资源
		ShutdownInitializedResources(false);
		runtime_state = NativeRuntimeState::Shutdown;
		throw;
	}
}

void MusicPlayerLibrary::NativeLibraryRuntime::Shutdown() noexcept
{
	std::lock_guard lock(runtime_mutex);
	if (runtime_state == NativeRuntimeState::Shutdown)
		return;

	if (runtime_state == NativeRuntimeState::Initialized)
	{
		runtime_state = NativeRuntimeState::ShuttingDown;
		NATIVE_TRACE("info: native library runtime shutting down");

		// shutdown了以后拒绝任何歌词推理请求
		ShutdownInitializedResources(true);
	}

	// 销毁ATLTRACE 重定向器
	if (runtime_state != NativeRuntimeState::ShuttingDown)
		ShutdownOutputAndTrace();
	runtime_state = NativeRuntimeState::Shutdown;
}

bool MusicPlayerLibrary::NativeLibraryRuntime::IsInitialized() noexcept
{
	std::lock_guard lock(runtime_mutex);
	return runtime_state == NativeRuntimeState::Initialized;
}
