// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Managed/LrcFileControllerManaged.h"
#include "Managed/LocaleConverterManaged.h"
#include "Core/LocaleConverter.h"

namespace
{
    System::InvalidOperationException^ ToManagedLrcException(const std::exception& exception)
    {
        const std::string message = exception.what();
        return gcnew System::InvalidOperationException(
            gcnew System::String(message.c_str(), 0, static_cast<int>(message.size()),
                System::Text::Encoding::UTF8));
    }
}

namespace MusicPlayerLibrary
{
    /*
     * Notice:
     * C++ LRC Module now doesn't contain state machine.
     * It only parses LRC files to Intermediate JSON Format.
     */
    LrcFileControllerManaged::LrcFileControllerManaged()
    {
        native_handle = new LrcFileController();
    }

    LrcFileControllerManaged::LrcFileControllerManaged(int songEndTimeMs)
    {
        native_handle = new LrcFileController(songEndTimeMs);
    }

    void LrcFileControllerManaged::check_if_null()
    {
        if (!native_handle)
            throw gcnew System::InvalidOperationException("LrcFileControllerNative is not initialized!");
    }

    System::String^ LrcFileControllerManaged::ParseLrcToIntermediateJson(System::String^ lrcString)
    {
        check_if_null();
        pin_ptr<const wchar_t> wch = PtrToStringChars(lrcString);
        const std::string utf8Str = LocaleConverter::GetUtf8StringFromUtf16String(std::wstring(wch));
        auto mem_file = GetDefaultFileSystem().CreateMemoryFile();
        if (!mem_file)
            return nullptr;
        mem_file->Write(utf8Str.data(), static_cast<std::uint32_t>(utf8Str.size()));
        mem_file->SeekToBegin();
        try
        {
            native_handle->parse_lrc_file_stream(mem_file.get());
        }
        catch (const std::exception& exception)
        {
            throw ToManagedLrcException(exception);
        }
        const auto json_utf8 = native_handle->to_intermediate_json();
        return LocaleConverterManaged::GetSystemStringFromUtf8String(json_utf8);
    }

    LrcFileControllerManaged::~LrcFileControllerManaged()
    {
        delete native_handle;
        native_handle = nullptr;
        System::GC::SuppressFinalize(this);
    }

    LrcFileControllerManaged::!LrcFileControllerManaged()
    {
        if (native_handle)
            delete native_handle;
        native_handle = nullptr;
    }
}
