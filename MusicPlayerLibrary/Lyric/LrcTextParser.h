// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MusicPlayerLibrary::LrcTextParser
{
    struct InlineTranslation
    {
        std::string original;
        std::string translation;
    };

    struct ProgressSegment
    {
        int end_time_ms = 0;
        std::string text;
    };

    struct ProgressLine
    {
        std::string plain_text;
        std::vector<ProgressSegment> segments;
    };

    struct ProgressLines
    {
        std::vector<std::string> plain_texts;
        std::optional<ProgressLine> controller_line;
        std::size_t controller_line_index = 0;
    };

    [[nodiscard]] std::string TrimAsciiWhitespace(std::string_view value);

    // Parses an LRC timestamp without its surrounding [] or <> delimiters.
    // Both ':' and '.' are accepted as separators. A fractional component is
    // optional and may contain one to four digits.
    [[nodiscard]] std::optional<int> TryParseTimestamp(std::string_view value) noexcept;

    [[nodiscard]] std::optional<ProgressLine> TryParseProgressLine(
        std::string_view value);

    [[nodiscard]] ProgressLines ParseProgressLines(
        std::span<const std::string> lines);

    [[nodiscard]] std::optional<InlineTranslation> TryParseSlashInlineTranslation(
        std::string_view value);

    [[nodiscard]] std::vector<std::string> SplitInlineTranslationText(
        std::string_view value,
        bool slash_translation_enabled);
}
