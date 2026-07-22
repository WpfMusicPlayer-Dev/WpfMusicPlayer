#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace MusicPlayerLibrary
{
    class IFile;
}

namespace MusicPlayerLibrary::MLPipeline
{
    inline constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
    inline constexpr std::uint64_t fnv_prime = 1099511628211ull;

    std::runtime_error format_error(
        const std::wstring& path,
        const std::string_view message);

    [[nodiscard]] std::string display_path(const std::wstring& path);

    [[nodiscard]] std::unique_ptr<IFile> open_read_file(
        const std::wstring& path,
        std::string_view description);

    [[nodiscard]] std::vector<unsigned char> read_binary_file(
        const std::wstring& path,
        std::string_view description,
        bool require_non_empty = true);

    void read_exact(
        IFile& input,
        char* destination,
        const std::size_t size,
        const std::wstring& path);

    std::uint32_t read_u32_le(
        IFile& input,
        const std::wstring& path);

    std::uint64_t read_u64_le(
        IFile& input,
        const std::wstring& path);

    void fingerprint_bytes(
        std::uint64_t& fingerprint,
        const char* bytes,
        const std::size_t size);

    std::uint64_t begin_bundle_fingerprint(
        const std::uint64_t model_fingerprint,
        const std::uint32_t entry_count);

    void fingerprint_token(std::uint64_t& fingerprint, const std::string_view token);

    void fingerprint_file(
        std::uint64_t& fingerprint,
        const std::string_view label,
        const std::wstring& path);

    std::size_t read_ncnn_input_size(const std::wstring& path);
}
