#include "pch.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "Core/FileAbstractionLayer.h"
#include "Lyric/MLPipeline/VocabularyIO.h"
#include "Lyric/MLPipeline/MLPipelineCommon.h"

namespace
{
    constexpr std::array<char, 8> VocabularyMagic = {
        'W', 'M', 'P', 'V', 'O', 'C', 'A', 'B'
    };
    constexpr std::uint32_t VocabularyFormatVersion = 1;
    constexpr std::size_t VocabularyHeaderSize =
        VocabularyMagic.size() + sizeof(std::uint32_t) * 2 + sizeof(std::uint64_t);
}

namespace MusicPlayerLibrary::MLPipeline
{
    Vocabulary load_vocab(
        const std::wstring& path,
        const std::uint64_t expected_model_fingerprint)
    {
        auto input = open_read_file(path, "vocabulary");

        std::array<char, VocabularyMagic.size()> magic{};
        read_exact(*input, magic.data(), magic.size(), path);
        if (magic != VocabularyMagic)
            throw format_error(path, "unrecognized magic");

        const auto version = read_u32_le(*input, path);
        if (version != VocabularyFormatVersion)
            throw format_error(path, "unsupported version " + std::to_string(version));

        const auto entry_count = read_u32_le(*input, path);
        if (entry_count == 0 || entry_count > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            throw format_error(path, "invalid entry count");
        const auto stored_bundle_fingerprint = read_u64_le(*input, path);

        const auto file_size = input->GetLength();
        if (file_size < VocabularyHeaderSize ||
            entry_count > (file_size - VocabularyHeaderSize) / (minimum_token_size + 1))
        {
            throw format_error(path, "entry count exceeds the file size");
        }

        Vocabulary vocabulary;
        vocabulary.reserve(entry_count);
        auto actual_bundle_fingerprint =
            begin_bundle_fingerprint(expected_model_fingerprint, entry_count);
        for (std::uint32_t index = 0; index < entry_count; ++index)
        {
            char length_byte{};
            read_exact(*input, &length_byte, 1, path);
            const auto token_size = static_cast<unsigned char>(length_byte);
            if (token_size < minimum_token_size || token_size > maximum_token_size)
                throw format_error(path, "token length is outside the supported range");

            std::string token(token_size, '\0');
            read_exact(*input, token.data(), token.size(), path);
            fingerprint_token(actual_bundle_fingerprint, token);
            if (!vocabulary.emplace(std::move(token), static_cast<int>(index)).second)
                throw format_error(path, "duplicate token");
        }

        if (input->GetPosition() != file_size)
            throw format_error(path, "unexpected trailing data");
        if (actual_bundle_fingerprint != stored_bundle_fingerprint)
            throw format_error(path, "model/vocabulary fingerprint mismatch");

        return vocabulary;
    }
}
