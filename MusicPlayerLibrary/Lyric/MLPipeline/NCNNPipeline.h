#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace MusicPlayerLibrary::MLPipeline
{
    inline constexpr std::string_view NcnnInputBlobName = "in0";

    struct NcnnModelFiles
    {
        std::wstring param;
        std::wstring weights;
    };

    class NcnnClassifier
    {
    public:
        explicit NcnnClassifier(const NcnnModelFiles& files);
        ~NcnnClassifier();

        NcnnClassifier(const NcnnClassifier&) = delete;
        NcnnClassifier& operator=(const NcnnClassifier&) = delete;
        NcnnClassifier(NcnnClassifier&&) noexcept;
        NcnnClassifier& operator=(NcnnClassifier&&) noexcept;

        std::size_t input_size() const noexcept;
        std::uint64_t model_fingerprint() const noexcept;
        int predict(std::span<const float> features) const;

    private:
        std::vector<float> run(std::span<const float> features) const;

        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
