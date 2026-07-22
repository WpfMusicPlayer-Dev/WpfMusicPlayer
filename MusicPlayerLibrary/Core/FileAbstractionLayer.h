// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace MusicPlayerLibrary {
	
	inline constexpr uint64_t SeekFailure = static_cast<uint64_t>(-1);

	enum class FileSeekOrigin
	{
		Begin,
		Current,
		End
	};

	class IFile
	{
	public:
		virtual ~IFile() = default;

		virtual uint32_t Read(void* buffer, uint32_t count) = 0;
		virtual void Write(const void* buffer, uint32_t count) = 0;
		virtual uint64_t Seek(int64_t offset, FileSeekOrigin origin) = 0;
		virtual void SeekToBegin()
		{
			(void)Seek(0, FileSeekOrigin::Begin);
		}
		virtual uint64_t GetLength() const = 0;
		virtual uint64_t GetPosition() const = 0;
		virtual void Close() = 0;
		virtual bool GetReadBuffer(void** buffer_start, void** buffer_end)
		{
			if (buffer_start != nullptr)
				*buffer_start = nullptr;
			if (buffer_end != nullptr)
				*buffer_end = nullptr;
			return false;
		}

		IFile() = default;
		IFile(const IFile&) = delete;
		IFile& operator=(const IFile&) = delete;
	};

	class IFileSystem
	{
	public:
		virtual ~IFileSystem() = default;

		virtual bool FileExists(const std::wstring& file_path) const = 0;
		virtual std::wstring GetFileExtension(const std::wstring& path) = 0;
		virtual std::unique_ptr<IFile> OpenReadFile(const std::wstring& file_path, bool share_deny_write, bool binary) const = 0;
		virtual std::unique_ptr<IFile> CreateTemporaryFile(bool binary) const = 0;
		virtual std::unique_ptr<IFile> CreateMemoryFile() const = 0;
	};

	enum class FileSystemImplementation
	{
		WindowsApi,
		FastIO
	};

	IFileSystem& GetBuiltInFileSystem(FileSystemImplementation implementation);
	FileSystemImplementation GetDefaultFileSystemImplementation();
	void SetDefaultFileSystemImplementation(
		FileSystemImplementation implementation);
	IFileSystem& GetDefaultFileSystem();
	void SetDefaultFileSystem(IFileSystem* file_system);

}
