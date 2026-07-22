// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace MusicPlayerLibrary::AudioDsp
{
    inline constexpr std::size_t EqualizerBandCount = 10;
    inline constexpr std::array<float, EqualizerBandCount>
        EqualizerBandFrequenciesHz{
            31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
            1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
        };
    inline constexpr std::uint32_t EqualizerSnapshotAbiVersion = 2;
	inline constexpr int MinimumEqualizerBandGainDb = -24;
	inline constexpr int MaximumEqualizerBandGainDb = 24;
	inline constexpr float MinimumEqualizerPreGain = 0.0f;
	inline constexpr float MaximumEqualizerPreGain = 4.0f;

    enum class BiquadType : std::uint32_t { Peaking = 0 };
    struct EqualizerBandConfig
    {
        BiquadType type = BiquadType::Peaking;
        float frequency_hz = 1000.0f;
        float q = 1.0f;
        float gain_db = 0.0f;
        bool enabled = true;
    };
    struct EqualizerConfig
    {
        std::array<EqualizerBandConfig, EqualizerBandCount> bands{};
    };
    struct BiquadCoefficients
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
    };
    struct EqualizerDspSnapshot
    {
        std::uint32_t abi_version = EqualizerSnapshotAbiVersion;
        std::uint32_t byte_size = 0;
        std::uint64_t reset_generation = 0;
        std::uint32_t enabled_mask = 0;
        // Sink-owned gain applied before EQ and limiting. Source-side PCM and
        // observers therefore remain normalized but otherwise unprocessed.
        float pre_gain = 1.0f;
        std::array<BiquadCoefficients, EqualizerBandCount> bands{};
    };
    struct LimiterConfig
    {
        bool enabled = true;
        float ceiling = 1.0f;
        float lookahead_ms = 5.0f;
        float release_ms = 50.0f;
        // Match the final limited output toward the decoded input's running
        // RMS, with feed-forward EQ compensation and limiter anti-windup.
        // This is enabled only for sink EQ processing.
        bool match_input_loudness = false;
    };
    static_assert(std::is_trivially_copyable_v<EqualizerDspSnapshot>);

    EqualizerConfig MakeDefaultTenBandConfig() noexcept;
	[[nodiscard]] LimiterConfig MakeSinkLimiterConfig() noexcept;
	[[nodiscard]] bool IsValidEqualizerSnapshotHeader(
		const EqualizerDspSnapshot& snapshot) noexcept;
    EqualizerDspSnapshot CompileEqualizerSnapshot(
        const EqualizerConfig&,
        std::uint32_t,
        std::uint64_t,
        float pre_gain = 1.0f) noexcept;

    class EqualizerDsp final
    {
    public:
        bool Prepare(std::uint32_t, std::uint32_t, std::uint32_t,
                     const LimiterConfig&);
        void Reset() noexcept;
        bool Process(const EqualizerDspSnapshot&, const float*, float*,
                     std::uint32_t, bool) noexcept;
        [[nodiscard]] bool HasTail() const noexcept;
        [[nodiscard]] std::uint32_t GetLimiterDelayFrames() const noexcept;
    private:
		void UpdateLoudnessMatchGain(float target_gain) noexcept;

        struct BiquadState { float z1 = 0.0f, z2 = 0.0f; };
        struct PeakNode
        {
            std::uint64_t frame_index = 0;
            float peak = 0.0f;
        };
        std::uint32_t sample_rate_ = 0;
        std::uint32_t channel_count_ = 0;
        std::uint32_t max_frame_count_ = 0;
        std::uint64_t applied_reset_generation_ = ~std::uint64_t{0};
        LimiterConfig limiter_{};
        std::uint32_t lookahead_window_frames_ = 1;
        std::uint32_t limiter_delay_frames_ = 0;
        std::uint32_t limiter_release_frames_ = 1;
        std::uint64_t limiter_frame_index_ = 0;
        std::uint64_t silent_input_frames_ = 0;
        std::size_t delay_write_frame_ = 0;
        std::size_t peak_head_ = 0;
        std::size_t peak_tail_ = 0;
        float limiter_gain_ = 1.0f;
        float limiter_release_step_ = 0.0f;
        std::uint32_t limiter_release_frames_remaining_ = 0;
        float input_loudness_power_ = 0.0f;
        float equalized_loudness_power_ = 0.0f;
		float delayed_input_loudness_power_ = 0.0f;
		float output_loudness_power_ = 0.0f;
        float loudness_match_gain_ = 1.0f;
        float loudness_power_history_ = 0.0f;
        float loudness_gain_history_ = 0.0f;
		std::uint64_t loudness_measurement_frames_ = 0;
        bool has_tail_ = false;
        std::vector<BiquadState> biquad_states_;
        std::vector<float> delay_line_;
		std::vector<float> input_power_delay_line_;
        std::vector<PeakNode> peak_queue_;
    };
}
