// SPDX-License-Identifier: MIT

#pragma once
#include "pch.h"
#include "Core/FileAbstractionLayer.h"

namespace MusicPlayerLibrary
{
    struct DecryptResult
    {
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        std::wstring ext;
        std::wstring pictureUrl;
        std::wstring mime;
    };

    struct NcmOpenResult
    {
        DecryptResult metadata;
        std::unique_ptr<IFile> audio_file;
    };

    class NcmDecryptor
    {
    public:
		static NcmOpenResult Open(std::unique_ptr<IFile> source_file);

    private:
		struct NcmMusicMeta
		{
			std::wstring musicName;
			std::vector<std::vector<std::wstring>> artist;
			std::wstring format;
			std::wstring album;
			std::wstring albumPic;
		};

		explicit NcmDecryptor(IFile& source_file);

        IFile& m_sourceFile;
        uint64_t m_fileLength = 0;
        uint64_t m_offset = 0;
        void EnsureSourceRange(uint64_t offset, uint64_t count, const char* message) const;
		void ReadSourceExactAt(
			uint64_t offset,
			void* buffer,
			uint32_t count,
			const char* message);
        uint32_t ReadSourceUint32(const char* message);
        uint32_t ReadSourceUint32At(uint64_t offset, const char* message);
        std::vector<uint8_t> ReadSourceBytes(uint32_t count, const char* message);
        std::vector<uint8_t> GetKeyData();
        std::vector<uint8_t> GetKeyBox();
        NcmMusicMeta GetMetaData();
        uint64_t GetAudioOffset();
		static DecryptResult BuildDecryptResult(const NcmMusicMeta& metadata);
    };
}
