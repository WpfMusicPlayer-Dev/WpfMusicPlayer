// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Managed/MusicPlayerEventBridge.h"
#include "Managed/MusicPlayerManaged.h"

namespace MusicPlayerLibrary
{
    MusicPlayerEventBridge::MusicPlayerEventBridge(MusicPlayerManaged^ managed_player)
        : managed_player_(gcnew System::WeakReference(managed_player))
    {
    }

    void MusicPlayerEventBridge::Publish(const PlayerMessage& message)
    {
        auto managed_player = dynamic_cast<MusicPlayerManaged^>(managed_player_->Target);
        if (managed_player == nullptr)
            return;

        System::Object^ payload = nullptr;
        switch (message.type)
        {
        case PlayerMessageType::TimeChanged:
            if (const auto time = std::get_if<double>(&message.payload))
                payload = *time;
            break;
        case PlayerMessageType::AlbumArtInitialized:
            if (const auto image = std::get_if<std::vector<std::uint8_t>>(&message.payload))
            {
                auto encoded_image = gcnew array<System::Byte>(static_cast<int>(image->size()));
                for (int index = 0; index < encoded_image->Length; ++index)
                    encoded_image[index] = (*image)[static_cast<std::size_t>(index)];
                payload = encoded_image;
            }
            break;
        case PlayerMessageType::NcmAlbumArtDownloadRequired:
            if (const auto url = std::get_if<std::wstring>(&message.payload))
                payload = gcnew System::String(url->c_str());
            break;
        case PlayerMessageType::Error:
            if (const auto error = std::get_if<std::string>(&message.payload))
            {
                payload = gcnew System::InvalidOperationException(
                    gcnew System::String(error->c_str(), 0, static_cast<int>(error->size()),
                        System::Text::Encoding::UTF8));
            }
            break;
        default:
            break;
        }

        managed_player->ProcessEvent(static_cast<int>(message.type), payload);
    }

#if defined(_WIN32)
    void MusicPlayerEventBridge::OnAudioOutputDeviceChanged() noexcept
    {
        try
        {
            Publish({PlayerMessageType::AudioOutputDeviceChanged});
        }
        catch (System::Exception^ exception)
        {
            System::Diagnostics::Trace::TraceError(
                System::String::Concat(
                    "Audio output device notification dispatch failed: ",
                    exception->ToString()));
        }
        catch (...)
        {
            NATIVE_TRACE(
                "warn: audio output device notification dispatch failed\n");
        }
    }
#endif
}
