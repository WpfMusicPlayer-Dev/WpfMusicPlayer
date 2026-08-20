// SPDX-License-Identifier: MIT

#pragma once

#include <vcclr.h>

#include "Audio/MusicPlayerMessage.h"
#if defined(_WIN32)
#include "Audio/Pipeline/Device/Windows/AudioOutputDeviceNotification.h"
#endif

namespace MusicPlayerLibrary
{
    ref class MusicPlayerManaged;
    
    class MusicPlayerEventBridge final : public IMusicPlayerMessageSink
#if defined(_WIN32)
        , public IAudioOutputDeviceChangeSink
#endif
    {
        gcroot<System::WeakReference^> managed_player_;

    public:
        explicit MusicPlayerEventBridge(MusicPlayerManaged^ managed_player);
        void Publish(const PlayerMessage& message) override;
#if defined(_WIN32)
        void OnAudioOutputDeviceChanged() noexcept override;
#endif
    };
}
