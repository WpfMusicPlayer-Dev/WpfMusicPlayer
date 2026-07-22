// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Audio/Pipeline/Sink/Common/FapoEqualizer.h"
#include "Audio/AudioOutputFormat.h"

#include <FAPOBase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace MusicPlayerLibrary::AudioDsp
{
    namespace
    {
        constexpr std::uint32_t RegistrationFlags =
            FAPO_FLAG_CHANNELS_MUST_MATCH |
            FAPO_FLAG_FRAMERATE_MUST_MATCH |
            FAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
            FAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
            FAPO_FLAG_INPLACE_SUPPORTED;
		const FAPORegistrationProperties RegistrationProperties{
            {0x876956c2, 0x8f29, 0x4af4,
             {0x9b, 0x61, 0x42, 0xd0, 0xd3, 0xac, 0x7a, 0x31}},
            {'W', 'p', 'f', ' ', 'E', 'q', 'u', 'a', 'l', 'i', 'z', 'e', 'r', 0},
            {'C', 'o', 'p', 'y', 'r', 'i', 'g', 'h', 't', ' ', 'W', 'p', 'f',
             'M', 'u', 's', 'i', 'c', 'P', 'l', 'a', 'y', 'e', 'r', 0},
            1,
            0,
            RegistrationFlags,
            1,
            1,
            1,
            1
        };

        struct EqualizerFapo final
        {
            FAPOBase base{};
            std::array<EqualizerDspSnapshot, 3> parameter_blocks{};
            EqualizerDsp dsp{};
            LimiterConfig limiter{};
            std::uint32_t channel_count = 0;
            std::uint32_t max_frame_count = 0;
			bool processing_enabled = false;
            std::atomic_bool tail_active{false};
            std::atomic<std::uint64_t> process_sequence{0};
        };

        static_assert(offsetof(EqualizerFapo, base) == 0);
        static_assert(sizeof(EqualizerDspSnapshot) <=
                      (std::numeric_limits<std::uint32_t>::max)());

        [[nodiscard]] EqualizerFapo* FromFapo(void* fapo) noexcept
        {
            return static_cast<EqualizerFapo*>(fapo);
        }

        [[nodiscard]] bool IsExactFloat32Extensible(
            const FAudioWaveFormatEx* format) noexcept
        {
            if (format == nullptr ||
                format->wFormatTag != FAUDIO_FORMAT_EXTENSIBLE ||
                format->nChannels < FAPO_MIN_CHANNELS ||
                format->nChannels > FAPO_MAX_CHANNELS ||
                format->nSamplesPerSec < FAPO_MIN_FRAMERATE ||
                format->nSamplesPerSec > FAPO_MAX_FRAMERATE ||
                format->wBitsPerSample != 32 ||
				format->cbSize != FAudioExtensibleFormatExtraSize)
            {
                return false;
            }

            const auto* extensible =
                reinterpret_cast<const FAudioWaveFormatExtensible*>(format);
            const auto expectedBlockAlign = static_cast<std::uint16_t>(
                format->nChannels * sizeof(float));
            const auto expectedAverageBytes = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(format->nSamplesPerSec) *
                expectedBlockAlign);
            return format->nBlockAlign == expectedBlockAlign &&
                format->nAvgBytesPerSec == expectedAverageBytes &&
                extensible->Samples.wValidBitsPerSample == 32 &&
				MusicPlayerLibrary::GuidEquals(
					extensible->SubFormat, IeeeFloatAudioSubFormat);
        }

        [[nodiscard]] bool FormatsMatchExactly(
            const FAudioWaveFormatEx* input,
            const FAudioWaveFormatEx* output) noexcept
        {
            return std::memcmp(
                input, output, sizeof(FAudioWaveFormatExtensible)) == 0;
        }

        std::uint32_t FAPOCALL IsFormatPairSupportedCallback(
            void* fapo,
            const FAudioWaveFormatEx* first,
            const FAudioWaveFormatEx* second,
            FAudioWaveFormatEx**) noexcept
        {
            if (fapo == nullptr || first == nullptr || second == nullptr)
                return FAUDIO_E_INVALID_ARG;
            if (!IsExactFloat32Extensible(first) ||
                !IsExactFloat32Extensible(second) ||
                !FormatsMatchExactly(first, second))
            {
                return FAPO_E_FORMAT_UNSUPPORTED;
            }
            return FAUDIO_OK;
        }

        void FAPOCALL DestroyCallback(void* fapo) noexcept
        {
            delete FromFapo(fapo);
        }

        void FAPOCALL ResetCallback(void* fapo) noexcept
        {
            if (fapo != nullptr)
            {
                auto* equalizer = FromFapo(fapo);
                equalizer->dsp.Reset();
                equalizer->tail_active.store(false, std::memory_order_release);
            }
        }

        std::uint32_t FAPOCALL LockForProcessCallback(
            void* fapo,
            std::uint32_t inputCount,
            const FAPOLockForProcessBufferParameters* inputParameters,
            std::uint32_t outputCount,
            const FAPOLockForProcessBufferParameters* outputParameters) noexcept
        {
            if (fapo == nullptr || inputCount != 1 || outputCount != 1 ||
                inputParameters == nullptr || outputParameters == nullptr)
            {
                return FAUDIO_E_INVALID_ARG;
            }

            auto* equalizer = FromFapo(fapo);
            if (inputParameters[0].MaxFrameCount == 0 ||
                inputParameters[0].MaxFrameCount !=
                    outputParameters[0].MaxFrameCount)
            {
                return FAUDIO_E_INVALID_ARG;
            }
            if (!IsExactFloat32Extensible(inputParameters[0].pFormat) ||
                !IsExactFloat32Extensible(outputParameters[0].pFormat))
            {
                return FAPO_E_FORMAT_UNSUPPORTED;
            }
            if (!FormatsMatchExactly(
                    inputParameters[0].pFormat, outputParameters[0].pFormat))
            {
                return FAUDIO_E_INVALID_ARG;
            }

            const auto baseResult = FAPOBase_LockForProcess(
                &equalizer->base,
                inputCount,
                inputParameters,
                outputCount,
                outputParameters);
            if (baseResult != FAUDIO_OK)
                return baseResult;

            try
            {
                if (!equalizer->dsp.Prepare(
                        inputParameters[0].pFormat->nSamplesPerSec,
                        inputParameters[0].pFormat->nChannels,
                        inputParameters[0].MaxFrameCount,
                        equalizer->limiter))
                {
                    FAPOBase_UnlockForProcess(&equalizer->base);
                    return FAUDIO_E_INVALID_ARG;
                }
            }
            catch (const std::bad_alloc&)
            {
                FAPOBase_UnlockForProcess(&equalizer->base);
                return FAUDIO_E_OUT_OF_MEMORY;
            }
            catch (...)
            {
                FAPOBase_UnlockForProcess(&equalizer->base);
                return FAUDIO_E_FAIL;
            }

            equalizer->channel_count = inputParameters[0].pFormat->nChannels;
            equalizer->max_frame_count = inputParameters[0].MaxFrameCount;
			equalizer->processing_enabled = false;
            equalizer->tail_active.store(false, std::memory_order_release);
            return FAUDIO_OK;
        }

        void FAPOCALL SetParametersCallback(
            void* fapo,
            const void* parameters,
            std::uint32_t parameterByteSize) noexcept
        {
            if (fapo == nullptr || parameters == nullptr ||
                parameterByteSize != sizeof(EqualizerDspSnapshot))
            {
                return;
            }

            const auto* snapshot =
                static_cast<const EqualizerDspSnapshot*>(parameters);
			if (!IsValidEqualizerSnapshotHeader(*snapshot))
                return;
            FAPOBase_SetParameters(
                &FromFapo(fapo)->base, parameters, parameterByteSize);
        }

        void FAPOCALL GetParametersCallback(
            void* fapo,
            void* parameters,
            std::uint32_t parameterByteSize) noexcept
        {
            if (fapo == nullptr || parameters == nullptr ||
                parameterByteSize != sizeof(EqualizerDspSnapshot))
            {
                return;
            }
            FAPOBase_GetParameters(
                &FromFapo(fapo)->base, parameters, parameterByteSize);
        }

        void FAPOCALL ProcessCallback(
            void* fapo,
            std::uint32_t inputCount,
            const FAPOProcessBufferParameters* inputParameters,
            std::uint32_t outputCount,
            FAPOProcessBufferParameters* outputParameters,
            std::int32_t isEnabled) noexcept
        {
            if (outputCount == 1 && outputParameters != nullptr)
            {
                outputParameters[0].BufferFlags = FAPO_BUFFER_SILENT;
                outputParameters[0].ValidFrameCount = 0;
            }
            if (fapo == nullptr || inputCount != 1 || outputCount != 1 ||
                inputParameters == nullptr || outputParameters == nullptr)
            {
                return;
            }

            auto* equalizer = FromFapo(fapo);
            const auto& input = inputParameters[0];
            auto& output = outputParameters[0];
            const bool inputSilent = input.BufferFlags == FAPO_BUFFER_SILENT;
            const bool inputValid = input.BufferFlags == FAPO_BUFFER_VALID;
            const bool hasFrames = input.ValidFrameCount != 0;
            if ((!inputSilent && !inputValid) ||
                equalizer->base.m_fIsLocked == 0 ||
                equalizer->channel_count == 0 ||
                equalizer->max_frame_count == 0 ||
                input.ValidFrameCount > equalizer->max_frame_count ||
                (hasFrames && output.pBuffer == nullptr) ||
                (hasFrames && inputValid && input.pBuffer == nullptr))
            {
                return;
            }

            output.ValidFrameCount = input.ValidFrameCount;
            const auto sampleCount =
                static_cast<std::size_t>(input.ValidFrameCount) *
                equalizer->channel_count;
            auto* outputSamples = static_cast<float*>(output.pBuffer);

            if (isEnabled == 0)
            {
                if (inputSilent)
                {
                    if (outputSamples != nullptr && output.pBuffer != input.pBuffer)
                        std::fill_n(outputSamples, sampleCount, 0.0f);
                    output.BufferFlags = FAPO_BUFFER_SILENT;
                }
                else if (input.pBuffer != nullptr && outputSamples != nullptr)
                {
                    if (output.pBuffer != input.pBuffer)
                    {
                        std::memmove(
                            output.pBuffer,
                            input.pBuffer,
                            sampleCount * sizeof(float));
                    }
                    output.BufferFlags = FAPO_BUFFER_VALID;
                }
				if (equalizer->processing_enabled)
					equalizer->dsp.Reset();
				equalizer->processing_enabled = false;
                equalizer->tail_active.store(false, std::memory_order_release);
                return;
            }

			equalizer->processing_enabled = true;
            const auto* snapshot = reinterpret_cast<const EqualizerDspSnapshot*>(
                FAPOBase_BeginProcess(&equalizer->base));
            bool outputActive = false;
            if (snapshot != nullptr && outputSamples != nullptr)
            {
                outputActive = equalizer->dsp.Process(
                    *snapshot,
                    static_cast<const float*>(input.pBuffer),
                    outputSamples,
                    input.ValidFrameCount,
                    inputSilent);
            }
            equalizer->tail_active.store(
                equalizer->dsp.HasTail(), std::memory_order_release);
            if (!outputActive && outputSamples != nullptr)
                std::fill_n(outputSamples, sampleCount, 0.0f);
            output.BufferFlags = outputActive
                ? FAPO_BUFFER_VALID
                : FAPO_BUFFER_SILENT;
            FAPOBase_EndProcess(&equalizer->base);
            equalizer->process_sequence.fetch_add(1, std::memory_order_release);
        }
    }

    std::uint32_t CreateEqualizerFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter,
        FAPO** effect) noexcept
    {
        if (effect == nullptr)
            return FAUDIO_E_INVALID_ARG;
        *effect = nullptr;
		if (!IsValidEqualizerSnapshotHeader(initial))
            return FAUDIO_E_INVALID_ARG;

        try
        {
            auto* equalizer = new (std::nothrow) EqualizerFapo{};
            if (equalizer == nullptr)
                return FAUDIO_E_OUT_OF_MEMORY;

            equalizer->parameter_blocks.fill(initial);
            equalizer->limiter = limiter;
            CreateFAPOBase(
                &equalizer->base,
                &RegistrationProperties,
                reinterpret_cast<std::uint8_t*>(
                    equalizer->parameter_blocks.data()),
                sizeof(EqualizerDspSnapshot),
                0);
            equalizer->base.base.IsInputFormatSupported =
                IsFormatPairSupportedCallback;
            equalizer->base.base.IsOutputFormatSupported =
                IsFormatPairSupportedCallback;
            equalizer->base.base.Reset = ResetCallback;
            equalizer->base.base.LockForProcess = LockForProcessCallback;
            equalizer->base.base.Process = ProcessCallback;
            equalizer->base.base.SetParameters = SetParametersCallback;
            equalizer->base.base.GetParameters = GetParametersCallback;
            equalizer->base.Destructor = DestroyCallback;
            *effect = &equalizer->base.base;
            return FAUDIO_OK;
        }
        catch (const std::bad_alloc&)
        {
            return FAUDIO_E_OUT_OF_MEMORY;
        }
        catch (...)
        {
            return FAUDIO_E_FAIL;
        }
    }

    bool EqualizerFapoHasTail(FAPO* effect) noexcept
    {
        return effect != nullptr && FromFapo(effect)->tail_active.load(
            std::memory_order_acquire);
    }

    std::uint64_t EqualizerFapoProcessSequence(FAPO* effect) noexcept
    {
        return effect == nullptr ? 0 : FromFapo(effect)->process_sequence.load(
            std::memory_order_acquire);
    }
}
