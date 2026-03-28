#include "pch.h"
#include "FFTExecuter.h"

void MusicPlayerLibrary::FFTExecuter::AddSamplesToRingBuffer(uint8_t* samples, int sample_size)
{
    std::lock_guard ring_buffer_lock(ring_buffer_mutex);
    if (samples == nullptr || sample_size <= 0)
        return;

    for (int i = 0; i < sample_size; ++i)
    {
        spectrum_data_ring_buffer.push_back(samples[i]);
    }

    // ReSharper disable once CppDFALoopConditionNotUpdated
    while (spectrum_data_ring_buffer.size() > RING_BUFFER_MAX_SIZE)
    {
        spectrum_data_ring_buffer.pop_front();
    }
    // wake fft consumer thread
    ring_buffer_cv.notify_one();
}

int MusicPlayerLibrary::FFTExecuter::GetRingBufferSize() const
{
    std::lock_guard lock(ring_buffer_mutex);
    return static_cast<int>(spectrum_data_ring_buffer.size());
}

void MusicPlayerLibrary::FFTExecuter::ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output)
{
    const size_t bytes_per_frame = 4;  // 2 channels * 2 bytes
    const size_t frame_count = input.size() / bytes_per_frame;
    output.resize(frame_count);

    for (size_t i = 0; i < frame_count; ++i) {
        auto left = static_cast<int16_t>(input[i * 4] | (input[i * 4 + 1] << 8));
        auto right = static_cast<int16_t>(input[i * 4 + 2] | (input[i * 4 + 3] << 8));
        // mix 2 channels
        double sample = (static_cast<double>(left) + static_cast<double>(right)) / 2.0;
        // hamming window
        const double w = 0.53836 * (1.0 - cos(2.0 * M_PI * i / (frame_count - 1)));
        // normalize
        output[i] = (sample / 32768.0) * w;
    }
}

void MusicPlayerLibrary::FFTExecuter::DoFFT(const std::vector<double>& windowed_data, std::vector<float>& fft_result, kiss_fft_cfg fft_cfg)
{
    size_t n = windowed_data.size();
    if (n == 0) return;

    // padding imaginary part to zero
    // cause kissfft only supports real input
    for (size_t i = 0; i < n; ++i) {
        fft_in[i].r = windowed_data[i];
        fft_in[i].i = 0.0f;
    }
    // zero padding
    for (size_t i = n; i < fft_size; ++i) {
        fft_in[i].r = 0.0;
        fft_in[i].i = 0.0;
    }

    kiss_fft(fft_cfg, fft_in.data(), fft_out.data());

    // 幅度谱
    fft_result.resize(fft_size / 2);
    for (size_t i = 0; i < fft_size / 2; ++i) {
        float real = fft_out[i].r;
        float imag = fft_out[i].i;
        fft_result[i] = sqrtf(real * real + imag * imag);
    }
}

std::vector<size_t> MusicPlayerLibrary::FFTExecuter::GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo, float f_hi)
{
    std::vector<size_t> boundaries(segment_num + 1);
    float delta_f = sample_rate / fft_size;
    size_t max_bin = fft_size / 2; // 只用正频率部分

    for (size_t i = 0; i <= segment_num; ++i) {
        float fraction = static_cast<float>(i) / segment_num;
        float freq = f_lo * pow(f_hi / f_lo, fraction);        // 对数插值
        auto idx = static_cast<size_t>(freq / delta_f);
        if (idx > max_bin - 1) idx = max_bin - 1;
        boundaries[i] = idx;
    }
    // 去重保证范围非空（即每个段至少1bin）
    for (size_t i = 1; i < boundaries.size(); ++i)
        if (boundaries[i] <= boundaries[i - 1]) boundaries[i] = boundaries[i - 1] + 1;
    if (boundaries.back() > max_bin) boundaries.back() = max_bin;
    return boundaries;
}

void MusicPlayerLibrary::FFTExecuter::MapFreqToSegments(
    std::vector<float>& fft_result,
    std::vector<float>& segments,
    const std::vector<size_t>& bandBounds)
{
    size_t numSegments = bandBounds.size() - 1;
    segments.resize(numSegments);
    for (size_t i = 0; i < numSegments; ++i) {
        float maxVal = 0.0f;
        for (size_t j = bandBounds[i]; j < bandBounds[i + 1]; ++j) {
            if (j >= fft_result.size()) break;
            if (fft_result[j] > maxVal) maxVal = fft_result[j];
        }
        segments[i] = maxVal;
    }
}

void MusicPlayerLibrary::FFTExecuter::ExecuteAudioFFT()
{

    std::vector<uint8_t> raw_samples;
    {
        std::lock_guard lock(ring_buffer_mutex);
        // 检查缓冲区是否有足够数据
        if (spectrum_data_ring_buffer.size() < RING_BUFFER_MAX_SIZE)
            return;

        // drain data
        raw_samples.assign(
            spectrum_data_ring_buffer.begin(),
            spectrum_data_ring_buffer.begin() + RING_BUFFER_MAX_SIZE);
    }

    // windowing
    std::vector<double> windowed;
    ApplyWindow(raw_samples, windowed);
    if (windowed.empty())
        return;

    // FFT
    std::vector<float> fft_result;
    DoFFT(windowed, fft_result, fft_cfg);

    if (fft_result.empty())
        return;

    // customizable sample rate, 32 segments
    constexpr size_t segment_num = 32;

    auto boundaries = GenBoundaries(sample_rate, fft_size, segment_num);

    {
        std::lock_guard<std::mutex> lock(spectrum_data_mutex); spectrum_data.clear();
        MapFreqToSegments(fft_result, spectrum_data, boundaries);

        for (size_t i = 0; i < spectrum_data.size(); ++i) {
            float& val = spectrum_data[i];
            // transition db
            float db = 20.0f * log10f(val + 1e-6f);
            constexpr float db_min = 10.0f;   // supress noise
            constexpr float db_max = 45.0f;   // full
            val = (db - db_min) / (db_max - db_min);
            if (val < 0.0f) val = 0.0f;
            if (val > 1.0f) val = 1.0f;

            // high freq attenuation
            constexpr size_t high_freq_start = segment_num * 2 / 3;
            if (i >= high_freq_start) {
                float attenuation = 1.0f - 0.4f * static_cast<float>(i - high_freq_start) / (segment_num - high_freq_start);
                val *= attenuation;
            }
        }

        // time-domain smoothing
        if (spectrum_smooth_data.size() != spectrum_data.size()) {
            spectrum_smooth_data.resize(spectrum_data.size(), 0.0f);
        }

        for (size_t i = 0; i < spectrum_data.size(); ++i) {
            float smooth_factor = 0.75f;
            spectrum_smooth_data[i] = smooth_factor * spectrum_smooth_data[i]
                + (1.0f - smooth_factor) * spectrum_data[i];
        }

        spectrum_data = spectrum_smooth_data;
    }
   
}

const std::vector<float> MusicPlayerLibrary::FFTExecuter::GetAudioFFTData()
{
    std::lock_guard<std::mutex> lock(spectrum_data_mutex);
    return spectrum_data;
}

void MusicPlayerLibrary::FFTExecuter::StartFFTThread()
{
    if (fft_thread_running.exchange(true))
        return;

    fft_worker_thread = std::thread(&FFTExecuter::FFTWorkerLoop, this);
}

void MusicPlayerLibrary::FFTExecuter::StopFFTThread()
{
    if (!fft_thread_running.exchange(false))
        return;

    ring_buffer_cv.notify_all();
    if (fft_worker_thread.joinable())
        fft_worker_thread.join();
    
    spectrum_data.clear();
    spectrum_smooth_data.clear();
    spectrum_data_ring_buffer.clear();
    fft_in.clear();
    fft_out.clear();
}

void MusicPlayerLibrary::FFTExecuter::FFTWorkerLoop()
{
    while (fft_thread_running)
    {
        std::unique_lock lock(ring_buffer_mutex);
        // FFT execution limitd to 60fps
        ring_buffer_cv.wait_for(lock, std::chrono::milliseconds(16), [this]()
            {
                return !fft_thread_running || spectrum_data_ring_buffer.size() >= RING_BUFFER_MAX_SIZE;
            });

        if (!fft_thread_running)
            break;

        if (spectrum_data_ring_buffer.size() < RING_BUFFER_MAX_SIZE)
            continue;

        lock.unlock();
        ExecuteAudioFFT();
    }
}

MusicPlayerLibrary::FFTExecuter::FFTExecuter(int in_sample_rate):
    sample_rate(in_sample_rate)
{
    fft_cfg = kiss_fft_alloc(fft_size, 0, nullptr, nullptr);
    if (!fft_cfg)
        throw std::runtime_error("kiss_fft_alloc failed!");

    fft_in.resize(fft_size);
    fft_out.resize(fft_size);
    StartFFTThread();
}

MusicPlayerLibrary::FFTExecuter::~FFTExecuter()
{
    StopFFTThread();
    if (fft_cfg)
    {
        kiss_fft_free(fft_cfg);
        fft_cfg = nullptr;
    }
}
