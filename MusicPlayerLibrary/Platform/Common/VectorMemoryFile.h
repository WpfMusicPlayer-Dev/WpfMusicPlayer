// SPDX-License-Identifier: MIT

#pragma once
#include "Core/FileAbstractionLayer.h"

namespace MusicPlayerLibrary
{
    class VectorMemoryFile final : public IFile
    {
    public:
        uint32_t Read(void* buffer, uint32_t count) override;

        void Write(const void* buffer, uint32_t count) override;

        uint64_t Seek(int64_t offset, FileSeekOrigin origin) override;

        uint64_t GetLength() const override;

        uint64_t GetPosition() const override;

        void Close() override;

        bool GetReadBuffer(void** buffer_start, void** buffer_end) override;

    private:
        std::vector<uint8_t> data_;
        uint64_t position_ = 0;
    };
}
