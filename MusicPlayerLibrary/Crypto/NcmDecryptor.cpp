// SPDX-License-Identifier: MIT

#include "pch.h"

#include <openssl/evp.h>
#include <cpp-base64/base64.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "Crypto/NcmDecryptor.h"
#include "Crypto/NcmFormat.h"
#include "Core/BinaryData.h"
#include "Core/FileIO.h"
#include "Core/FileSeek.h"
#include "Core/LocaleConverter.h"

namespace
{
constexpr std::size_t Aes128KeySize = 16;
constexpr std::size_t Aes128BlockSize = 16;
constexpr std::size_t NcmKeyBoxSize = 256;
constexpr std::size_t NcmKeyBoxMask = NcmKeyBoxSize - 1;
constexpr std::size_t KeyDataPrefixSize = 17;
constexpr std::size_t MetadataBase64PrefixSize = 22;
static_assert((NcmKeyBoxSize & NcmKeyBoxMask) == 0);

using Aes128Key = std::array<std::uint8_t, Aes128KeySize>;

constexpr Aes128Key CoreKey{
    0x68, 0x7a, 0x48, 0x52, 0x41, 0x6d, 0x73, 0x6f,
    0x35, 0x6b, 0x49, 0x6e, 0x62, 0x61, 0x78, 0x57
};
constexpr Aes128Key MetadataKey{
    0x23, 0x31, 0x34, 0x6C, 0x6A, 0x6B, 0x5F, 0x21,
    0x5C, 0x5D, 0x26, 0x30, 0x55, 0x3C, 0x27, 0x28
};
std::vector<uint8_t> Aes128EcbDecrypt(
    const std::vector<uint8_t>& cipher,
    const Aes128Key& key)
{
    if (cipher.size() > (std::numeric_limits<std::size_t>::max)() -
            Aes128BlockSize ||
        cipher.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)()))
    {
        throw std::length_error("AES input is too large");
    }

    std::vector<uint8_t> plain(cipher.size() + Aes128BlockSize);
    using UniqueCipherContext = std::unique_ptr<
        EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    UniqueCipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    EVP_CIPHER_CTX* ctx = context.get();
    if (!ctx)
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    int outlen1 = 0, outlen2 = 0;
    if (EVP_DecryptInit_ex(
        ctx, EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1)
    {
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }
    EVP_CIPHER_CTX_set_padding(ctx, 1);
    if (EVP_DecryptUpdate(ctx, plain.data(), &outlen1, cipher.data(), static_cast<int>(cipher.size())) != 1)
    {
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    if (EVP_DecryptFinal_ex(ctx, plain.data() + outlen1, &outlen2) != 1)
    {
        throw std::runtime_error("EVP_DecryptFinal_ex failed");
    }
    plain.resize(outlen1 + outlen2);
    return plain;
}
    class NcmDecryptedAudioFile final : public MusicPlayerLibrary::IFile
    {
    public:
        NcmDecryptedAudioFile(
            std::unique_ptr<MusicPlayerLibrary::IFile> source_file,
            uint64_t audio_offset,
            uint64_t audio_length,
            std::vector<uint8_t> key_box)
            : source_file_(std::move(source_file)),
            audio_offset_(audio_offset),
            audio_length_(audio_length),
            key_box_(std::move(key_box))
        {
            if (!source_file_)
                throw std::runtime_error("invalid ncm source file");
            if (key_box_.size() != NcmKeyBoxSize)
                throw std::runtime_error("invalid ncm key box");
        }

        uint32_t Read(void* buffer, uint32_t count) override
        {
            if (!source_file_ || buffer == nullptr || count == 0 || position_ >= audio_length_)
                return 0;

            const uint64_t bytes_to_read_64 =
                std::min(static_cast<uint64_t>(count), audio_length_ - position_);
            const uint32_t bytes_to_read = static_cast<uint32_t>(bytes_to_read_64);
            const uint64_t source_position = audio_offset_ + position_;
			if (source_position < audio_offset_)
            {
                NATIVE_TRACE("err: ncm decrypted audio source position overflow\n");
                return 0;
            }

			if (!MusicPlayerLibrary::TrySeekFileAbsolute(
				*source_file_, source_position))
            {
                NATIVE_TRACE("err: seek ncm audio source failed\n");
                return 0;
            }

            auto* bytes = static_cast<uint8_t*>(buffer);
            const uint32_t bytes_read = source_file_->Read(bytes, bytes_to_read);
            for (uint32_t i = 0; i < bytes_read; ++i)
                bytes[i] ^= key_box_[(position_ + i) & NcmKeyBoxMask];

            position_ += bytes_read;
            return bytes_read;
        }

        void Write(const void* buffer, uint32_t count) override
        {
            (void)buffer;
            (void)count;
            NATIVE_TRACE("err: ncm decrypted audio file is read-only\n");
        }

        uint64_t Seek(int64_t offset, MusicPlayerLibrary::FileSeekOrigin origin) override
        {
            uint64_t new_position;
            if (!MusicPlayerLibrary::TryResolveFileSeekPosition(
                position_, audio_length_, offset, origin, new_position))
            {
                if (offset < 0)
                    NATIVE_TRACE("err: ncm decrypted audio seek before begin\n");
                else
                    NATIVE_TRACE("err: ncm decrypted audio seek position overflow\n");
                return MusicPlayerLibrary::SeekFailure;
            }

            position_ = new_position;
            return position_;
        }

        uint64_t GetLength() const override
        {
            return audio_length_;
        }

        uint64_t GetPosition() const override
        {
            return position_;
        }

        void Close() override
        {
			source_file_.reset();
            key_box_.clear();
            audio_offset_ = 0;
            audio_length_ = 0;
            position_ = 0;
        }

    private:
        std::unique_ptr<IFile> source_file_;
        uint64_t audio_offset_ = 0;
        uint64_t audio_length_ = 0;
        uint64_t position_ = 0;
        std::vector<uint8_t> key_box_;
    };
}

void MusicPlayerLibrary::NcmDecryptor::EnsureSourceRange(uint64_t offset, uint64_t count, const char* message) const
{
    if (offset > m_fileLength || count > m_fileLength - offset)
        throw std::runtime_error(message);
}

void MusicPlayerLibrary::NcmDecryptor::ReadSourceExactAt(
	const uint64_t offset,
	void* const buffer,
	const uint32_t count,
	const char* const message)
{
	EnsureSourceRange(offset, count, message);
	if (!TrySeekFileAbsolute(m_sourceFile, offset) ||
		!ReadExact(m_sourceFile, buffer, count))
        throw std::runtime_error(message);
}

uint32_t MusicPlayerLibrary::NcmDecryptor::ReadSourceUint32(const char* message)
{
    const auto result = ReadSourceUint32At(m_offset, message);
    m_offset += sizeof(std::uint32_t);
    return result;
}

uint32_t MusicPlayerLibrary::NcmDecryptor::ReadSourceUint32At(uint64_t offset, const char* message)
{
	std::array<std::uint8_t, sizeof(std::uint32_t)> bytes{};
	ReadSourceExactAt(offset, bytes.data(), bytes.size(), message);
    return DecodeLittleEndian<uint32_t>(bytes);
}

std::vector<uint8_t> MusicPlayerLibrary::NcmDecryptor::ReadSourceBytes(uint32_t count, const char* message)
{
    std::vector<uint8_t> bytes(count);
    if (count > 0)
		ReadSourceExactAt(m_offset, bytes.data(), count, message);
	else
		EnsureSourceRange(m_offset, 0, message);
    m_offset += count;
    return bytes;
}

static std::wstring JsonStringToWideString(const rapidjson::Value& value)
{
    if (!value.IsString())
        return {};
    return MusicPlayerLibrary::LocaleConverter::GetUtf16StringFromUtf8String(
        std::string(value.GetString(), value.GetStringLength()));
}

static std::wstring JsonNumberToWideString(const rapidjson::Value& value)
{
    std::string number;
    if (value.IsInt64())
        number = std::to_string(value.GetInt64());
    else if (value.IsUint64())
        number = std::to_string(value.GetUint64());
    else if (value.IsNumber())
        number = std::to_string(value.GetDouble());
    else
        return {};

    return MusicPlayerLibrary::LocaleConverter::GetUtf16StringFromUtf8String(number);
}

MusicPlayerLibrary::NcmDecryptor::NcmDecryptor(IFile& source_file)
	: m_sourceFile(source_file), m_fileLength(source_file.GetLength())
{
	std::array<char, NcmMagicHeader.size()> header{};
	ReadSourceExactAt(
		0, header.data(), header.size(), "failed to read ncm header");
	if (!IsNcmMagicHeader(header))
        throw std::runtime_error("此ncm文件已损坏");
	m_offset = NcmKeySectionOffset;
}

MusicPlayerLibrary::NcmOpenResult MusicPlayerLibrary::NcmDecryptor::Open(
	std::unique_ptr<IFile> source_file)
{
    if (!source_file)
        throw std::runtime_error("invalid ncm source file");

	NcmDecryptor decryptor(*source_file);
	auto keyBox = decryptor.GetKeyBox();
	const auto metadata = decryptor.GetMetaData();
    const uint64_t audioOffset = decryptor.GetAudioOffset();
    const uint64_t audioLength = decryptor.m_fileLength - audioOffset;

    NcmOpenResult openResult;
	openResult.metadata = BuildDecryptResult(metadata);
    openResult.audio_file = std::make_unique<NcmDecryptedAudioFile>(
        std::move(source_file),
        audioOffset,
        audioLength,
        std::move(keyBox));
    return openResult;
}

MusicPlayerLibrary::DecryptResult MusicPlayerLibrary::NcmDecryptor::BuildDecryptResult(
	const NcmMusicMeta& metadata)
{
	const std::wstring format = metadata.format.empty() ? L"mp3" : metadata.format;
	const std::wstring mime = format == L"mp3"
		? L"audio/mpeg"
		: format == L"flac" ? L"audio/flac" : L"application/octet-stream";
	DecryptResult res;
	res.ext = format;
	res.mime = mime;
	res.title = metadata.musicName;
	res.album = metadata.album;
	res.pictureUrl = metadata.albumPic;
	std::wstring artistJoined;
	for (const auto& arr : metadata.artist)
    {
        if (!arr.empty())
        {
            if (!artistJoined.empty()) artistJoined += L"; ";
            artistJoined += arr[0];
        }
    }
    res.artist = artistJoined;
    return res;
}

std::vector<uint8_t> MusicPlayerLibrary::NcmDecryptor::GetKeyData()
{
    uint32_t keyLen = ReadSourceUint32("invalid ncm key length");
    std::vector<uint8_t> cipherText = ReadSourceBytes(keyLen, "invalid ncm key area");
    for (auto& byte : cipherText)
        byte ^= 0x64;
    auto plain = Aes128EcbDecrypt(cipherText, CoreKey);
    if (plain.size() <= KeyDataPrefixSize)
        throw std::runtime_error("key data too short");
    std::vector result(plain.begin() + KeyDataPrefixSize, plain.end());
    return result;
}

std::vector<uint8_t> MusicPlayerLibrary::NcmDecryptor::GetKeyBox()
{
    std::vector<uint8_t> keyData = GetKeyData();
    size_t keyLen = keyData.size();
    if (keyLen == 0)
        throw std::runtime_error("empty keyData");
    std::array<std::uint8_t, NcmKeyBoxSize> box{};
    for (std::size_t i = 0; i < NcmKeyBoxSize; ++i)
        box[i] = static_cast<uint8_t>(i);
    uint8_t j = 0;
    for (std::size_t i = 0; i < NcmKeyBoxSize; ++i)
    {
        j = static_cast<uint8_t>(
            (box[i] + j + keyData[i % keyLen]) & NcmKeyBoxMask);
        std::swap(box[i], box[j]);
    }
    std::vector<uint8_t> keyBox(NcmKeyBoxSize);
    for (std::size_t i = 0; i < NcmKeyBoxSize; ++i)
    {
        const std::size_t idx = (i + 1) & NcmKeyBoxMask;
        uint8_t si = box[idx];
        uint8_t sj = box[(idx + si) & NcmKeyBoxMask];
        keyBox[i] = box[(si + sj) & NcmKeyBoxMask];
    }
    return keyBox;
}

MusicPlayerLibrary::NcmDecryptor::NcmMusicMeta
MusicPlayerLibrary::NcmDecryptor::GetMetaData()
{
    uint32_t metaDataLen = ReadSourceUint32("invalid meta length");
    if (metaDataLen == 0)
        return NcmMusicMeta{};
    std::vector<uint8_t> metaData = ReadSourceBytes(metaDataLen, "invalid meta area");
    for (auto& byte : metaData)
        byte ^= 0x63;
    if (metaData.size() <= MetadataBase64PrefixSize)
        throw std::runtime_error("meta data too short");
    std::string base64Str(
        metaData.begin() + MetadataBase64PrefixSize, metaData.end());
    const std::string decoded_text = base64_decode(base64Str);
    const std::vector<std::uint8_t> decoded(
        decoded_text.begin(), decoded_text.end());
    auto plain = Aes128EcbDecrypt(decoded, MetadataKey);
    std::string text(plain.begin(), plain.end());
    size_t labelIndex = text.find(':');
    if (labelIndex == std::string::npos)
        throw std::runtime_error("invalid meta text");
    std::string label = text.substr(0, labelIndex);
    std::string jsonStr = text.substr(labelIndex + 1);
    using namespace rapidjson;
    Document doc;
    doc.Parse(jsonStr.c_str());
    if (doc.HasParseError())
    {
        throw std::runtime_error(std::string("meta json parse error: ") + GetParseError_En(doc.GetParseError()));
    }
    if (!doc.IsObject())
        throw std::runtime_error("meta json root is not object");
    NcmMusicMeta result;
    auto parseMusicMeta = [&](const Value& v, NcmMusicMeta& out)
        {
            if (!v.IsObject())
                return;

            auto readStringMember = [](const Value& source, const char* name, std::wstring& dest)
                {
                    const auto member = source.FindMember(name);
                    if (member != source.MemberEnd() && member->value.IsString())
                        dest = JsonStringToWideString(member->value);
                };
            auto appendJsonValue = [](const Value& item, std::vector<std::wstring>& dest)
                {
                    std::wstring value;
                    if (item.IsString())
                        value = JsonStringToWideString(item);
                    else if (item.IsNumber())
                        value = JsonNumberToWideString(item);

                    if (!value.empty())
                        dest.emplace_back(value);
                };

            readStringMember(v, "musicName", out.musicName);
            readStringMember(v, "album", out.album);
            readStringMember(v, "format", out.format);
            readStringMember(v, "albumPic", out.albumPic);

            const auto artistMember = v.FindMember("artist");
            if (artistMember != v.MemberEnd() && artistMember->value.IsArray())
            {
                const auto& arr = artistMember->value;
                for (SizeType i = 0; i < arr.Size(); ++i)
                {
                    std::vector<std::wstring> oneArtist;
                    const auto& item = arr[i];
                    if (item.IsArray())
                    {
                        for (SizeType j = 0; j < item.Size(); ++j)
                            appendJsonValue(item[j], oneArtist);
                    }
                    else
                        appendJsonValue(item, oneArtist);

                    if (!oneArtist.empty())
                        out.artist.push_back(std::move(oneArtist));
                }
            }
        };
    if (label == "dj")
    {
        const auto mainMusic = doc.FindMember("mainMusic");
        if (mainMusic == doc.MemberEnd() || !mainMusic->value.IsObject())
            throw std::runtime_error(
                "dj meta missing mainMusic");
        parseMusicMeta(mainMusic->value, result);
    }
    else { parseMusicMeta(doc, result); }
    if (!result.albumPic.empty())
    {
        // if (result.albumPic.rfind("http://", 0) == 0) result.albumPic.replace(0, 7, "https://");
        if (result.albumPic.rfind(L"http://", 0) == 0)
            result.albumPic = std::wstring(L"https://") + result.albumPic.substr(7);
        result.albumPic += L"?param=500y500";
    }
    std::wstring firstArtist;
    if (!result.artist.empty() && !result.artist[0].empty())
        firstArtist = result.artist[0][0];
    std::wstring jsonTrace = LocaleConverter::GetUtf16StringFromUtf8String(jsonStr);

    NATIVE_TRACE(L"info: NcmDecrypt success!\n");
    NATIVE_TRACE(L"info: music name: %s\n", result.musicName.c_str());
    NATIVE_TRACE(L"info: album: %s\n", result.album.c_str());
    NATIVE_TRACE(L"info: artist: %s\n", firstArtist.c_str());
    NATIVE_TRACE(L"info: album pic: %s\n", result.albumPic.c_str());
    NATIVE_TRACE(L"info: format: %s\n", result.format.c_str());
    NATIVE_TRACE(L"info: meta data: %s\n", jsonTrace.c_str());
    return result;
}

uint64_t MusicPlayerLibrary::NcmDecryptor::GetAudioOffset()
{
	EnsureSourceRange(
		m_offset, NcmAudioHeaderPrefixSize, "invalid audio offset");
	const uint32_t dataLen = ReadSourceUint32At(
		m_offset + NcmAudioDataLengthFieldOffset, "invalid audio offset");
    if (NcmAudioHeaderSize > m_fileLength - m_offset
        || static_cast<uint64_t>(dataLen) >
            m_fileLength - m_offset - NcmAudioHeaderSize)
        throw std::runtime_error("audio offset out of range");
    m_offset += static_cast<uint64_t>(dataLen) + NcmAudioHeaderSize;
    return m_offset;
}
