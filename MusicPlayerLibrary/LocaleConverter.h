#pragma once

#include "pch.h"

namespace MusicPlayerLibrary 
{
	class LocaleConverterNative
	{
	public:
		static CStringA GetUtf8StringFromBytesNative(const char* input, size_t size);
		static CString GetUtf16StringFromUtf8String(const CStringA& input);
	};

	public ref class LocaleConverter abstract sealed {
	public:
		static System::String^ GetSystemStringFromBytes(array<byte>^ input);
	};
}