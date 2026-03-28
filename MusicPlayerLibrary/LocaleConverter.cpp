#include "pch.h"
#include "AtlTraceRedirect.h"
#include "LocaleConverter.h"

#include <iconv.h>
#include <uchardet/uchardet.h>
#include <msclr/marshal_cppstd.h>

CStringA MusicPlayerLibrary::LocaleConverterNative::GetUtf8StringFromBytesNative(const char* input, size_t size)
{
    if (!input || size == 0) return "";

    auto uc_checker = uchardet_new();
    // 调用者负责约束input的有效性，因此不需要复制
    uchardet_handle_data(uc_checker, input, size);
    uchardet_data_end(uc_checker);
    const char* charset = uchardet_get_charset(uc_checker);
    ATLTRACE("info: detected charset = %s\n", charset);
    
    // if is utf-8 or ansi...
    // ansi is a subset of UTF-8
    // conversion guard
    if (!charset || strlen(charset) == 0 || _stricmp(charset, "UTF-8") == 0) {
        uchardet_delete(uc_checker);
        return CStringA(input, static_cast<int>(size));
    }

    iconv_t iconver = iconv_open("UTF-8", charset);
    // fix: release uc_checker
    uchardet_delete(uc_checker);

    if (iconver == reinterpret_cast<iconv_t>(-1)) {
        return CStringA(input, static_cast<int>(size));
    }

    size_t insize = size;
    // UTF-8 encoding may expand to 4*in_size
    size_t outcapacity = size * 4; 
    size_t outleft = outcapacity;
    
    CStringA outbuf;
    char* pOriginalStart = outbuf.GetBufferSetLength(static_cast<int>(outcapacity));
    char* pIn = const_cast<char*>(input);
    char* pOut = pOriginalStart;

    size_t res = iconv(iconver, &pIn, &insize, &pOut, &outleft);
    
    // fix: release iconver
    iconv_close(iconver); 

    if (res == static_cast<size_t>(-1)) {
        outbuf.ReleaseBuffer(0);
        return CStringA(input, static_cast<int>(size));
    }

    // Shrink actualSize to fit
    int actualSize = static_cast<int>(outcapacity - outleft);
    outbuf.ReleaseBufferSetLength(actualSize); 

    return outbuf;
}

CString MusicPlayerLibrary::LocaleConverterNative::GetUtf16StringFromUtf8String(const CStringA& input)
{
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, input, -1, nullptr, 0);
    CString file_content_w;
    MultiByteToWideChar(CP_UTF8, 0, input, -1, file_content_w.GetBuffer(wide_len), wide_len);
    file_content_w.ReleaseBuffer();
    return file_content_w;
}

System::String^ MusicPlayerLibrary::LocaleConverter::GetSystemStringFromBytes(array<byte>^ input)
{
    if (input == nullptr || input->Length == 0)
        return gcnew System::String(L"");

    std::vector<char> buffer(input->Length);
    for (int i = 0; i < input->Length; ++i)
        buffer[i] = input[i];

    CStringA utf8 = LocaleConverterNative::GetUtf8StringFromBytesNative(buffer.data(), buffer.size());
    CString s = LocaleConverterNative::GetUtf16StringFromUtf8String(utf8);

    while (!s.IsEmpty())
    {
        WCHAR ch = s[s.GetLength() - 1];
        if ((ch >= 0x20 && ch <= 0xD7FF) ||
            (ch >= 0xE000 && ch <= 0xFFFD))
        {
            break;
        }
        // 剔除不合法Unicode字符
        s.Truncate(s.GetLength() - 1);
    }

    return msclr::interop::marshal_as<System::String^>(s.GetString());
}

