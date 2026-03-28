using WpfMusicPlayer.Models;
using static WpfMusicPlayer.Services.Implementations.ConfigProvider;

namespace WpfMusicPlayer.Services.Abstractions
{
    public interface IConfigProvider
    {
        public ErrorCode CreateConfigFile(string ConfigFileName = "config.xml");
        public ErrorCode Reload(string ConfigFileName = "config.xml");
        public ErrorCode WriteFile(string ConfigFileName = "config.xml");
        public ref ConfigData GetConfig();
    }
}
