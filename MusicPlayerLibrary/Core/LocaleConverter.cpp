// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Core/LocaleConverter.h"
#include "Core/StringUtilities.h"
#include "Core/Utf16Conversion.h"

#include <iconv.h>
#include <uchardet/uchardet.h>

#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace
{
constexpr char Utf8Encoding[] = "UTF-8";
constexpr char Utf16LeEncoding[] = "UTF-16LE";

bool IsUtf8ContinuationByte(unsigned char value)
{
    return (value & 0xC0) == 0x80;
}

bool IsValidUtf8Bytes(const char* input, size_t size)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(input);
    size_t i = 0;
    while (i < size)
    {
        const unsigned char current = bytes[i];
        if (current <= 0x7F)
        {
            ++i;
            continue;
        }

        if (current >= 0xC2 && current <= 0xDF)
        {
            if (i + 1 >= size || !IsUtf8ContinuationByte(bytes[i + 1]))
                return false;
            i += 2;
            continue;
        }

        if (current == 0xE0)
        {
            if (i + 2 >= size
                || bytes[i + 1] < 0xA0
                || bytes[i + 1] > 0xBF
                || !IsUtf8ContinuationByte(bytes[i + 2]))
            {
                return false;
            }
            i += 3;
            continue;
        }

        if ((current >= 0xE1 && current <= 0xEC)
            || (current >= 0xEE && current <= 0xEF))
        {
            if (i + 2 >= size
                || !IsUtf8ContinuationByte(bytes[i + 1])
                || !IsUtf8ContinuationByte(bytes[i + 2]))
            {
                return false;
            }
            i += 3;
            continue;
        }

        if (current == 0xED)
        {
            if (i + 2 >= size
                || bytes[i + 1] < 0x80
                || bytes[i + 1] > 0x9F
                || !IsUtf8ContinuationByte(bytes[i + 2]))
            {
                return false;
            }
            i += 3;
            continue;
        }

        if (current == 0xF0)
        {
            if (i + 3 >= size
                || bytes[i + 1] < 0x90
                || bytes[i + 1] > 0xBF
                || !IsUtf8ContinuationByte(bytes[i + 2])
                || !IsUtf8ContinuationByte(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        if (current >= 0xF1 && current <= 0xF3)
        {
            if (i + 3 >= size
                || !IsUtf8ContinuationByte(bytes[i + 1])
                || !IsUtf8ContinuationByte(bytes[i + 2])
                || !IsUtf8ContinuationByte(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        if (current == 0xF4)
        {
            if (i + 3 >= size
                || bytes[i + 1] < 0x80
                || bytes[i + 1] > 0x8F
                || !IsUtf8ContinuationByte(bytes[i + 2])
                || !IsUtf8ContinuationByte(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        return false;
    }

    return true;
}

bool IsAsciiBytes(const char* input, size_t size)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(input);
    for (size_t i = 0; i < size; ++i)
    {
        if (bytes[i] > 0x7F)
            return false;
    }

    return true;
}

using UCharDetElement = std::remove_pointer_t<uchardet_t>;

struct UCharDetDeleter final
{
    void operator()(UCharDetElement* detector) const noexcept
    {
        if (detector != nullptr)
            uchardet_delete(detector);
    }
};

using UniqueUCharDet = std::unique_ptr<UCharDetElement, UCharDetDeleter>;

std::string DetectCharset(const char* input, const size_t size)
{
    UniqueUCharDet detector(uchardet_new());
    if (!detector || uchardet_handle_data(detector.get(), input, size) != 0)
        return {};

    uchardet_data_end(detector.get());
    const char* charset = uchardet_get_charset(detector.get());
    return charset != nullptr ? std::string(charset) : std::string{};
}

class IconvHandle final
{
    iconv_t value_;

public:
    IconvHandle(const char* destination_encoding, const char* source_encoding) noexcept :
        value_(iconv_open(destination_encoding, source_encoding))
    {
    }

    ~IconvHandle()
    {
        if (*this)
            iconv_close(value_);
    }

    IconvHandle(const IconvHandle&) = delete;
    IconvHandle& operator=(const IconvHandle&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != reinterpret_cast<iconv_t>(-1);
    }

    [[nodiscard]] iconv_t Get() const noexcept { return value_; }
};

std::optional<std::string> ConvertEncoding(
    const char* input,
    const size_t input_size,
    const char* destination_encoding,
    const char* source_encoding,
    const size_t output_capacity_multiplier)
{
    if (input_size == 0)
        return std::string{};
    if (input == nullptr || output_capacity_multiplier == 0 ||
        input_size > (std::numeric_limits<size_t>::max)() /
            output_capacity_multiplier)
    {
        return std::nullopt;
    }

    IconvHandle converter(destination_encoding, source_encoding);
    if (!converter)
        return std::nullopt;

    size_t input_left = input_size;
    const size_t output_capacity = input_size * output_capacity_multiplier;
    size_t output_left = output_capacity;
    std::string output(output_capacity, '\0');
    char* input_cursor = const_cast<char*>(input);
    char* output_cursor = output.data();
    if (iconv(
        converter.Get(), &input_cursor, &input_left,
        &output_cursor, &output_left) == static_cast<size_t>(-1))
    {
        return std::nullopt;
    }

    output.resize(output_capacity - output_left);
    return output;
}

bool IsUtf8CompatibleCharset(const std::string_view charset)
{
    if (charset.empty())
        return true;

    return MusicPlayerLibrary::EqualsAsciiIgnoreCase(charset, std::string_view(Utf8Encoding))
        || MusicPlayerLibrary::EqualsAsciiIgnoreCase(charset, std::string_view("ASCII"))
        || MusicPlayerLibrary::EqualsAsciiIgnoreCase(charset, std::string_view("US-ASCII"))
        || MusicPlayerLibrary::EqualsAsciiIgnoreCase(charset, std::string_view("ANSI"))
        || MusicPlayerLibrary::EqualsAsciiIgnoreCase(
			charset, std::string_view("ANSI_X3.4-1968"));
}
}

std::string MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromBytes(const char* input, size_t size)
{
    if (!input || size == 0) return {};

    const std::string charset = DetectCharset(input, size);
    NATIVE_TRACE("info: detected charset = %s\n", charset.c_str());
    
    // if is utf-8 or ansi...
    // ansi is a subset of UTF-8
    // conversion guard
	if (charset.empty() ||
		MusicPlayerLibrary::EqualsAsciiIgnoreCase(
			std::string_view(charset), std::string_view(Utf8Encoding))) {
        return std::string(input, size);
    }

	const auto converted = ConvertEncoding(
		input, size, Utf8Encoding, charset.c_str(), 4);
	return converted ? std::move(*converted) : std::string(input, size);
}

bool MusicPlayerLibrary::LocaleConverter::IsUtf8CompatibleBytes(const char* input, size_t size)
{
    if (!input || size == 0) return true;

    const std::string charset = DetectCharset(input, size);
    NATIVE_TRACE(
		"info: detected charset for UTF-8 validation = %s\n",
		charset.c_str());

    const bool charset_compatible = IsUtf8CompatibleCharset(charset);
    const bool bytes_compatible = IsValidUtf8Bytes(input, size);
    return bytes_compatible && (charset_compatible || IsAsciiBytes(input, size));
}

std::wstring MusicPlayerLibrary::LocaleConverter::GetUtf16StringFromUtf8String(const std::string& input)
{
    if (input.empty())
        return {};

	static_assert(sizeof(wchar_t) == 2);
	const auto converted = ConvertEncoding(
		input.data(), input.size(), Utf16LeEncoding, Utf8Encoding, 2);
	if (!converted || converted->size() % sizeof(wchar_t) != 0)
        return {};

	std::wstring result(converted->size() / sizeof(wchar_t), L'\0');
	std::memcpy(result.data(), converted->data(), converted->size());
	return result;
}

std::string MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(const std::wstring& input)
{
	static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
	return ConvertUtf16CodeUnitsToUtf8(
		input.data(), input.size(), false);
}
