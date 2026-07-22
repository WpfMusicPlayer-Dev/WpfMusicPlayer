// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Platform/Windows/WindowsApiDiskFile.h"

namespace MusicPlayerLibrary
{
	namespace
	{
		DWORD ToWindowsSeekOrigin(FileSeekOrigin origin)
		{
			switch (origin)
			{
			case FileSeekOrigin::Begin:
				return FILE_BEGIN;
			case FileSeekOrigin::Current:
				return FILE_CURRENT;
			case FileSeekOrigin::End:
				return FILE_END;
			default:
				return FILE_BEGIN;
			}
		}
	}

	bool WindowsApiDiskFile::Open(const std::wstring& file_path, bool share_deny_write)
	{
		Close();

		DWORD share_mode = FILE_SHARE_READ;
		if (!share_deny_write)
			share_mode |= FILE_SHARE_WRITE;

		file_.Reset(CreateFileW(
			file_path.c_str(),
			GENERIC_READ,
			share_mode,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr));
		if (!file_)
		{
			NATIVE_TRACE(L"err: CreateFileW failed:%ls, gle=%lu\n", file_path.c_str(), ::GetLastError());
			return false;
		}

		return true;
	}

	bool WindowsApiDiskFile::CreateTemporary()
	{
		Close();

		constexpr DWORD TempPathBufferLength = MAX_PATH + 1;
		wchar_t temp_path[TempPathBufferLength] = {};
		const DWORD temp_path_len = ::GetTempPathW(TempPathBufferLength, temp_path);
		if (temp_path_len == 0 || temp_path_len >= TempPathBufferLength)
		{
			NATIVE_TRACE("err: GetTempPathW failed, gle=%lu\n", ::GetLastError());
			return false;
		}

		wchar_t temp_file_path[TempPathBufferLength] = {};
		if (::GetTempFileNameW(temp_path, L"WMP", 0, temp_file_path) == 0)
		{
			NATIVE_TRACE("err: GetTempFileNameW failed, gle=%lu\n", ::GetLastError());
			return false;
		}

		file_.Reset(CreateFileW(
			temp_file_path,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
			nullptr));
		if (!file_)
		{
			NATIVE_TRACE(L"err: CreateFileW failed:%ls, gle=%lu\n", temp_file_path, ::GetLastError());
			::DeleteFileW(temp_file_path);
			return false;
		}

		return true;
	}

	uint32_t WindowsApiDiskFile::Read(void* buffer, uint32_t count)
	{
		if (!file_ || buffer == nullptr || count == 0)
			return 0;

		DWORD bytes_read = 0;
		if (!ReadFile(file_.Get(), buffer, count, &bytes_read, nullptr))
		{
			NATIVE_TRACE("err: ReadFile failed, gle=%lu\n", ::GetLastError());
			return 0;
		}

		return static_cast<uint32_t>(bytes_read);
	}

	void WindowsApiDiskFile::Write(const void* buffer, uint32_t count)
	{
		if (!file_ || buffer == nullptr || count == 0)
			return;

		DWORD bytes_written = 0;
		if (!WriteFile(file_.Get(), buffer, count, &bytes_written, nullptr)
			|| bytes_written != static_cast<DWORD>(count))
		{
			NATIVE_TRACE("err: WriteFile failed, gle=%lu\n", ::GetLastError());
		}
	}

	uint64_t WindowsApiDiskFile::Seek(int64_t offset, FileSeekOrigin origin)
	{
		if (!file_)
			return SeekFailure;

		LARGE_INTEGER distance = {
			.QuadPart = offset
		};
		LARGE_INTEGER new_position = {};
		if (!SetFilePointerEx(
			file_.Get(), distance, &new_position, ToWindowsSeekOrigin(origin)))
		{
			NATIVE_TRACE("err: SetFilePointerEx failed, gle=%lu\n", ::GetLastError());
			return SeekFailure;
		}

		return static_cast<uint64_t>(new_position.QuadPart);
	}

	uint64_t WindowsApiDiskFile::GetLength() const
	{
		if (!file_)
			return 0;

		LARGE_INTEGER file_size = {};
		if (!GetFileSizeEx(file_.Get(), &file_size))
		{
			NATIVE_TRACE("err: GetFileSizeEx failed, gle=%lu\n", ::GetLastError());
			return 0;
		}

		return static_cast<uint64_t>(file_size.QuadPart);
	}

	uint64_t WindowsApiDiskFile::GetPosition() const
	{
		if (!file_)
			return 0;

		LARGE_INTEGER distance = {};
		LARGE_INTEGER position = {};
		if (!SetFilePointerEx(file_.Get(), distance, &position, FILE_CURRENT))
		{
			NATIVE_TRACE("err: SetFilePointerEx failed, gle=%lu\n", ::GetLastError());
			return 0;
		}

		return static_cast<uint64_t>(position.QuadPart);
	}

	void WindowsApiDiskFile::Close()
	{
		file_.Reset();
	}

}
