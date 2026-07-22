// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Lyric/LrcTextParser.h"

#include <cctype>
#include <utility>

namespace
{
    constexpr std::string_view HalfWidthLeftParenthesis = "(";
    constexpr std::string_view HalfWidthRightParenthesis = ")";
    constexpr std::string_view FullWidthLeftParenthesis = "\xEF\xBC\x88";
    constexpr std::string_view FullWidthRightParenthesis = "\xEF\xBC\x89";
    constexpr std::string_view FullWidthColon = "\xEF\xBC\x9A";
    constexpr std::string_view TranslationMarker = "\xE7\xBF\xBB\xE8\xAF\x91";

    void SkipAsciiWhitespace(const std::string_view value, std::size_t& position) noexcept
    {
        while (position < value.size() &&
            std::isspace(static_cast<unsigned char>(value[position])) != 0)
        {
            ++position;
        }
    }

    std::optional<int> ReadDecimal(
        const std::string_view value,
        std::size_t& position,
        const std::size_t minimum_digits,
        const std::size_t maximum_digits) noexcept
    {
        const std::size_t begin = position;
        int result = 0;
        while (position < value.size() && position - begin < maximum_digits &&
            std::isdigit(static_cast<unsigned char>(value[position])) != 0)
        {
            result = result * 10 + (value[position] - '0');
            ++position;
        }

        if (position - begin < minimum_digits)
            return std::nullopt;
        return result;
    }

    bool TryReadParenthesisToken(
        const std::string_view value,
        const std::size_t position,
        bool& is_left,
        std::size_t& token_size) noexcept
    {
        const std::string_view remaining = value.substr(position);
        if (remaining.starts_with(FullWidthLeftParenthesis))
        {
            is_left = true;
            token_size = FullWidthLeftParenthesis.size();
            return true;
        }
        if (remaining.starts_with(FullWidthRightParenthesis))
        {
            is_left = false;
            token_size = FullWidthRightParenthesis.size();
            return true;
        }
        if (remaining.starts_with(HalfWidthLeftParenthesis))
        {
            is_left = true;
            token_size = HalfWidthLeftParenthesis.size();
            return true;
        }
        if (remaining.starts_with(HalfWidthRightParenthesis))
        {
            is_left = false;
            token_size = HalfWidthRightParenthesis.size();
            return true;
        }
        return false;
    }

    std::size_t FindMatchingParenthesisClose(
        const std::string_view value,
        const std::size_t open_position) noexcept
    {
        bool is_left = false;
        std::size_t token_size = 0;
        if (!TryReadParenthesisToken(value, open_position, is_left, token_size) || !is_left)
            return std::string_view::npos;

        int depth = 0;
        for (std::size_t position = open_position; position < value.size();)
        {
            if (!TryReadParenthesisToken(value, position, is_left, token_size))
            {
                ++position;
                continue;
            }

            if (is_left)
            {
                ++depth;
            }
            else
            {
                --depth;
                if (depth == 0)
                    return position;
                if (depth < 0)
                    return std::string_view::npos;
            }
            position += token_size;
        }
        return std::string_view::npos;
    }

    std::optional<MusicPlayerLibrary::LrcTextParser::InlineTranslation>
    TryParseBracketInlineTranslation(const std::string_view value)
    {
        using MusicPlayerLibrary::LrcTextParser::InlineTranslation;
        using MusicPlayerLibrary::LrcTextParser::TrimAsciiWhitespace;

        const std::string trimmed_text = TrimAsciiWhitespace(value);
        for (std::size_t position = 0; position < trimmed_text.size();)
        {
            bool is_left = false;
            std::size_t left_size = 0;
            if (!TryReadParenthesisToken(trimmed_text, position, is_left, left_size))
            {
                ++position;
                continue;
            }
            if (!is_left)
            {
                position += left_size;
                continue;
            }

            const std::size_t close_position =
                FindMatchingParenthesisClose(trimmed_text, position);
            if (close_position == std::string_view::npos)
            {
                position += left_size;
                continue;
            }

            bool close_is_left = false;
            std::size_t close_size = 0;
            if (!TryReadParenthesisToken(
                    trimmed_text, close_position, close_is_left, close_size) ||
                close_is_left || close_position + close_size != trimmed_text.size())
            {
                position += left_size;
                continue;
            }

            std::string original = TrimAsciiWhitespace(
                std::string_view(trimmed_text).substr(0, position));
            if (original.empty())
            {
                position += left_size;
                continue;
            }

            std::string inner = TrimAsciiWhitespace(
                std::string_view(trimmed_text).substr(
                    position + left_size,
                    close_position - position - left_size));
            if (!std::string_view(inner).starts_with(TranslationMarker))
            {
                position += left_size;
                continue;
            }

            std::string after_marker = TrimAsciiWhitespace(
                std::string_view(inner).substr(TranslationMarker.size()));
            std::size_t colon_size = 0;
            if (std::string_view(after_marker).starts_with(":"))
                colon_size = 1;
            else if (std::string_view(after_marker).starts_with(FullWidthColon))
                colon_size = FullWidthColon.size();
            else
            {
                position += left_size;
                continue;
            }

            std::string translation = TrimAsciiWhitespace(
                std::string_view(after_marker).substr(colon_size));
            if (!translation.empty())
                return InlineTranslation{std::move(original), std::move(translation)};

            position += left_size;
        }
        return std::nullopt;
    }
}

std::string MusicPlayerLibrary::LrcTextParser::TrimAsciiWhitespace(
    const std::string_view value)
{
    std::size_t first = 0;
    SkipAsciiWhitespace(value, first);

    std::size_t last = value.size();
    while (last > first &&
        std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
    {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::optional<int> MusicPlayerLibrary::LrcTextParser::TryParseTimestamp(
    const std::string_view value) noexcept
{
    std::size_t position = 0;
    SkipAsciiWhitespace(value, position);

    const auto minutes = ReadDecimal(value, position, 1, 2);
    if (!minutes)
        return std::nullopt;
    SkipAsciiWhitespace(value, position);
    if (position >= value.size() || (value[position] != ':' && value[position] != '.'))
        return std::nullopt;
    ++position;

    SkipAsciiWhitespace(value, position);
    const auto seconds = ReadDecimal(value, position, 1, 2);
    if (!seconds)
        return std::nullopt;
    SkipAsciiWhitespace(value, position);

    int milliseconds = 0;
    if (position < value.size())
    {
        if (value[position] != ':' && value[position] != '.')
            return std::nullopt;
        ++position;
        SkipAsciiWhitespace(value, position);

        const std::size_t fraction_begin = position;
        const auto fraction = ReadDecimal(value, position, 1, 4);
        if (!fraction)
            return std::nullopt;
        const std::size_t fraction_digits = position - fraction_begin;
        SkipAsciiWhitespace(value, position);
        if (position != value.size())
            return std::nullopt;

        milliseconds = *fraction;
        if (fraction_digits == 1)
            milliseconds *= 100;
        else if (fraction_digits == 2)
            milliseconds *= 10;
        else if (fraction_digits == 4)
            milliseconds /= 10;
    }

    if (position != value.size())
        return std::nullopt;
    return *minutes * 60'000 + *seconds * 1'000 + milliseconds;
}

std::optional<MusicPlayerLibrary::LrcTextParser::ProgressLine>
MusicPlayerLibrary::LrcTextParser::TryParseProgressLine(const std::string_view value)
{
    ProgressLine result;
    std::size_t segment_begin = 0;
    std::size_t search_position = 0;

    while (search_position < value.size())
    {
        const std::size_t open_position = value.find_first_of("<[", search_position);
        if (open_position == std::string_view::npos)
            break;

        const char open = value[open_position];
        const char expected_close = open == '<' ? '>' : ']';
        const char wrong_close = open == '<' ? ']' : '>';
        const std::size_t close_position = value.find(expected_close, open_position + 1);
        const std::size_t wrong_close_position = value.find(wrong_close, open_position + 1);

        if (wrong_close_position != std::string_view::npos &&
            (close_position == std::string_view::npos || wrong_close_position < close_position))
        {
            const auto mismatched_token = value.substr(
                open_position + 1,
                wrong_close_position - open_position - 1);
            if (TryParseTimestamp(mismatched_token))
                return std::nullopt;
        }

        if (close_position == std::string_view::npos)
        {
            search_position = open_position + 1;
            continue;
        }

        const auto timestamp = TryParseTimestamp(value.substr(
            open_position + 1,
            close_position - open_position - 1));
        if (!timestamp)
        {
            search_position = open_position + 1;
            continue;
        }

        std::string segment_text(value.substr(
            segment_begin,
            open_position - segment_begin));
        result.plain_text += segment_text;
        result.segments.push_back({*timestamp, std::move(segment_text)});
        segment_begin = close_position + 1;
        search_position = segment_begin;
    }

    if (result.segments.empty() || segment_begin != value.size())
        return std::nullopt;
    return result;
}

MusicPlayerLibrary::LrcTextParser::ProgressLines
MusicPlayerLibrary::LrcTextParser::ParseProgressLines(
    const std::span<const std::string> lines)
{
    ProgressLines result;
    result.plain_texts.reserve(lines.size());
    std::size_t best_segment_count = 0;

    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        auto parsed = TryParseProgressLine(lines[index]);
        if (!parsed)
        {
            result.plain_texts.push_back(lines[index]);
            continue;
        }

        result.plain_texts.push_back(parsed->plain_text);
        if (parsed->segments.size() > best_segment_count)
        {
            best_segment_count = parsed->segments.size();
            result.controller_line_index = index;
            result.controller_line = std::move(parsed);
        }
    }
    return result;
}

std::optional<MusicPlayerLibrary::LrcTextParser::InlineTranslation>
MusicPlayerLibrary::LrcTextParser::TryParseSlashInlineTranslation(
    const std::string_view value)
{
    constexpr std::string_view Delimiter = " / ";
    const std::size_t delimiter_position = value.find(Delimiter);
    if (delimiter_position == std::string_view::npos)
        return std::nullopt;

    InlineTranslation result{
        TrimAsciiWhitespace(value.substr(0, delimiter_position)),
        TrimAsciiWhitespace(value.substr(delimiter_position + Delimiter.size()))
    };
    if (result.original.empty() || result.translation.empty())
        return std::nullopt;
    return result;
}

std::vector<std::string> MusicPlayerLibrary::LrcTextParser::SplitInlineTranslationText(
    const std::string_view value,
    const bool slash_translation_enabled)
{
    auto split = TryParseBracketInlineTranslation(value);
    if (!split && slash_translation_enabled)
        split = TryParseSlashInlineTranslation(value);
    if (!split)
        return {std::string(value)};
    return {std::move(split->original), std::move(split->translation)};
}
