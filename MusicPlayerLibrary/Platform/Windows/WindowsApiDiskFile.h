// SPDX-License-Identifier: MIT

#pragma once
#include "Core/FileAbstractionLayer.h"
#include "Platform/Windows/WindowsResource.h"

namespace MusicPlayerLibrary
{
    class WindowsApiDiskFile final : public IFile
    {
    public:
        ~WindowsApiDiskFile() override = default;

        bool Open(const std::wstring& file_path, bool share_deny_write);

        bool CreateTemporary();

        uint32_t Read(void* buffer, uint32_t count) override;

        void Write(const void* buffer, uint32_t count) override;

        uint64_t Seek(int64_t offset, FileSeekOrigin origin) override;

        uint64_t GetLength() const override;

        uint64_t GetPosition() const override;

        void Close() override;

    private:
		UniqueHandle file_;
    };
}
