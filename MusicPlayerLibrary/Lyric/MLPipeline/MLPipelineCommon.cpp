#include "pch.h"
#include "Lyric/MLPipeline/MLPipelineCommon.h"
#include "Core/BinaryData.h"
#include "Core/FileAbstractionLayer.h"
#include "Core/FileIO.h"
#include "Core/LocaleConverter.h"
#include "Lyric/MLPipeline/NCNNPipeline.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <type_traits>
#include <vector>

namespace
{
    template <typename T>
    T ReadUnsignedIntegerLittleEndian(
        MusicPlayerLibrary::IFile& input,
        const std::wstring& path)
    {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);

        std::array<unsigned char, sizeof(T)> bytes{};
        MusicPlayerLibrary::MLPipeline::read_exact(
            input,
            reinterpret_cast<char*>(bytes.data()),
            bytes.size(),
            path);
        return MusicPlayerLibrary::DecodeLittleEndian<T>(bytes);
    }

    std::string ReadTextFile(
        const std::wstring& path,
        const std::string_view description)
    {
        const auto contents = MusicPlayerLibrary::MLPipeline::read_binary_file(
            path,
            description,
            false);
        return { contents.begin(), contents.end() };
    }

    template <typename T>
    void FingerprintUnsignedIntegerLittleEndian(std::uint64_t& fingerprint, const T value)
    {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);

        const auto bytes = MusicPlayerLibrary::EncodeLittleEndian(value);
        MusicPlayerLibrary::MLPipeline::fingerprint_bytes(
            fingerprint,
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size());
    }

    std::size_t ParsePositiveSize(
        const std::string_view text,
        const std::wstring& path,
        const std::string_view label)
    {
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
            value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        {
            throw std::runtime_error(
                "invalid " + std::string(label) + " in NCNN param file: " +
                MusicPlayerLibrary::MLPipeline::display_path(path));
        }
        return static_cast<std::size_t>(value);
    }
}

std::string MusicPlayerLibrary::MLPipeline::display_path(const std::wstring& path)
{
    const auto utf8_path = LocaleConverter::GetUtf8StringFromUtf16String(path);
    return utf8_path.empty() && !path.empty() ? "<unrepresentable path>" : utf8_path;
}

std::unique_ptr<MusicPlayerLibrary::IFile> MusicPlayerLibrary::MLPipeline::open_read_file(
    const std::wstring& path,
    const std::string_view description)
{
    auto file = GetDefaultFileSystem().OpenReadFile(path, true, true);
    if (!file)
    {
        throw std::runtime_error(
            "failed to open " + std::string(description) + ": " + display_path(path));
    }
    return file;
}

std::vector<unsigned char> MusicPlayerLibrary::MLPipeline::read_binary_file(
    const std::wstring& path,
    const std::string_view description,
    const bool require_non_empty)
{
    auto file = open_read_file(path, description);
    const auto length = file->GetLength();
    if (require_non_empty && length == 0)
        throw std::runtime_error(std::string(description) + " is empty: " + display_path(path));

    std::vector<unsigned char> contents;
    if (length > static_cast<std::uint64_t>(contents.max_size()))
        throw std::runtime_error(std::string(description) + " is too large: " + display_path(path));

    contents.resize(static_cast<std::size_t>(length));
    if (!ReadExact(*file, contents.data(), contents.size()))
    {
        throw std::runtime_error(
            "failed to read " + std::string(description) + ": " + display_path(path));
    }
    return contents;
}

std::runtime_error MusicPlayerLibrary::MLPipeline::format_error(const std::wstring& path,
    const std::string_view message)
{
    return std::runtime_error(
        "invalid vocabulary file '" + display_path(path) + "': " + std::string(message));
}

void MusicPlayerLibrary::MLPipeline::read_exact(IFile& input, char* destination, const std::size_t size,
    const std::wstring& path)
{
    if (!ReadExact(input, destination, size))
        throw format_error(path, "unexpected end of file");
}

std::uint32_t MusicPlayerLibrary::MLPipeline::read_u32_le(IFile& input, const std::wstring& path)
{
    return ReadUnsignedIntegerLittleEndian<std::uint32_t>(input, path);
}

std::uint64_t MusicPlayerLibrary::MLPipeline::read_u64_le(IFile& input, const std::wstring& path)
{
    return ReadUnsignedIntegerLittleEndian<std::uint64_t>(input, path);
}

void MusicPlayerLibrary::MLPipeline::fingerprint_bytes(std::uint64_t& fingerprint, const char* bytes,
    const std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        fingerprint ^= static_cast<unsigned char>(bytes[i]);
        fingerprint *= fnv_prime;
    }
}

std::uint64_t MusicPlayerLibrary::MLPipeline::begin_bundle_fingerprint(const std::uint64_t model_fingerprint,
    const std::uint32_t entry_count)
{
    std::uint64_t fingerprint = fnv_offset_basis;
    constexpr std::string_view domain = "WMP-VOCAB-BUNDLE-V1";
    fingerprint_bytes(fingerprint, domain.data(), domain.size());
    FingerprintUnsignedIntegerLittleEndian(fingerprint, model_fingerprint);
    FingerprintUnsignedIntegerLittleEndian(fingerprint, entry_count);
    return fingerprint;
}

void MusicPlayerLibrary::MLPipeline::fingerprint_token(std::uint64_t& fingerprint, const std::string_view token)
{
    const char size = static_cast<char>(token.size());
    fingerprint_bytes(fingerprint, &size, 1);
    fingerprint_bytes(fingerprint, token.data(), token.size());
}

void MusicPlayerLibrary::MLPipeline::fingerprint_file(std::uint64_t& fingerprint, const std::string_view label,
    const std::wstring& path)
{
    FingerprintUnsignedIntegerLittleEndian(
        fingerprint,
        static_cast<std::uint64_t>(label.size()));
    fingerprint_bytes(fingerprint, label.data(), label.size());

    auto input = open_read_file(path, "NCNN model file");
    const auto file_size = input->GetLength();
    FingerprintUnsignedIntegerLittleEndian(fingerprint, file_size);

    std::array<char, 64 * 1024> buffer{};
    std::uint64_t bytes_read{};
    while (bytes_read < file_size)
    {
        const auto request_size = static_cast<std::uint32_t>(std::min(
            static_cast<std::uint64_t>(buffer.size()),
            file_size - bytes_read));
        if (!ReadExact(*input, buffer.data(), request_size))
            throw std::runtime_error("failed to fingerprint NCNN model file: " + display_path(path));
        fingerprint_bytes(fingerprint, buffer.data(), request_size);
        bytes_read += request_size;
    }
}

std::size_t MusicPlayerLibrary::MLPipeline::read_ncnn_input_size(const std::wstring& path)
{
    const auto contents = ReadTextFile(path, "NCNN param file");
    const std::string input_blob_name(NcnnInputBlobName);
    std::istringstream input(contents);

    std::string magic;
    if (!(input >> magic) || magic != "7767517")
        throw std::runtime_error("invalid NCNN param magic: " + display_path(path));

    std::size_t layer_count{};
    std::size_t blob_count{};
    if (!(input >> layer_count >> blob_count) || layer_count == 0 || blob_count == 0)
        throw std::runtime_error("invalid NCNN layer/blob count: " + display_path(path));

    std::string line;
    std::getline(input, line);
    for (std::size_t layer_index = 0; layer_index < layer_count; ++layer_index)
    {
        if (!std::getline(input, line))
            throw std::runtime_error("truncated NCNN param file: " + display_path(path));

        std::istringstream layer_stream(line);
        std::string type;
        std::string name;
        int bottom_count{};
        int top_count{};
        if (!(layer_stream >> type >> name >> bottom_count >> top_count) ||
            bottom_count < 0 || top_count < 0 ||
            static_cast<std::size_t>(bottom_count) > blob_count ||
            static_cast<std::size_t>(top_count) > blob_count)
        {
            throw std::runtime_error("invalid NCNN layer record: " + display_path(path));
        }

        std::vector<std::string> bottoms(static_cast<std::size_t>(bottom_count));
        for (auto& bottom : bottoms)
        {
            if (!(layer_stream >> bottom))
                throw std::runtime_error("invalid NCNN bottom list: " + display_path(path));
        }
        for (int i = 0; i < top_count; ++i)
        {
            std::string ignored_top;
            if (!(layer_stream >> ignored_top))
                throw std::runtime_error("invalid NCNN top list: " + display_path(path));
        }

        if (type != "InnerProduct" ||
            std::find(bottoms.begin(), bottoms.end(), input_blob_name) == bottoms.end())
        {
            continue;
        }

        std::size_t output_size{};
        std::size_t weight_count{};
        for (std::string parameter; layer_stream >> parameter;)
        {
            if (parameter.starts_with("0="))
                output_size = ParsePositiveSize(
                    std::string_view(parameter).substr(2),
                    path,
                    "output size");
            else if (parameter.starts_with("2="))
                weight_count = ParsePositiveSize(
                    std::string_view(parameter).substr(2),
                    path,
                    "weight count");
        }

        if (output_size == 0 || weight_count == 0 || weight_count % output_size != 0)
            throw std::runtime_error("invalid input InnerProduct dimensions: " + display_path(path));
        return weight_count / output_size;
    }

    throw std::runtime_error(
        "NCNN model has no InnerProduct layer consuming '" + input_blob_name + "': " +
        display_path(path));
}
