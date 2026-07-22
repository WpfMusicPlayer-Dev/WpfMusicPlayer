#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace MusicPlayerLibrary::MLPipeline
{
    inline constexpr std::size_t minimum_token_size = 1;
    inline constexpr std::size_t maximum_token_size = 3;

    using Vocabulary = std::unordered_map<std::string, int>;

    // The on-disk format is independent of dlib:
    //   8 bytes  magic "WMPVOCAB"
    //   u32 LE   format version
    //   u32 LE   entry count
    //   u64 LE   bundle fingerprint (model + ordered vocabulary records)
    //   repeated in feature-index order: u8 byte_length, byte_length raw bytes
    Vocabulary load_vocab(
        const std::wstring& path,
        std::uint64_t expected_model_fingerprint);
}
