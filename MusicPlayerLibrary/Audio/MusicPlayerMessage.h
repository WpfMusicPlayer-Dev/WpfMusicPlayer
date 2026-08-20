// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace MusicPlayerLibrary
{
    enum class PlayerMessageType : std::uint32_t
    {
        FileInitialized = 100,
        TimeChanged = 101,
        Started = 102,
        Paused = 103,
        Stopped = 104,
        AlbumArtInitialized = 105,
        NcmAlbumArtDownloadRequired = 106,
        Destroyed = 107,
        Error = 108,
        AudioOutputDeviceChanged = 109
    };

    using PlayerMessagePayload = std::variant<
        std::monostate,
        double,
        std::vector<std::uint8_t>,
        std::wstring,
        std::string>;

    struct PlayerMessage
    {
        PlayerMessageType type;
        PlayerMessagePayload payload{};
    };

    class IMusicPlayerMessageSink
    {
    public:
        virtual ~IMusicPlayerMessageSink() = default;
        virtual void Publish(const PlayerMessage& message) = 0;
    };
}
