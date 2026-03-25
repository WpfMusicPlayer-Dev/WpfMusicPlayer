using WpfMusicPlayer.Models;
using static WpfMusicPlayer.Services.ConfigProvider;

namespace WpfMusicPlayer.Services
{
    public interface IConfigProvider
    {
        public ErrorCode CreateConfigFile(string ConfigFileName = "config.xml");
        public ErrorCode Reload(string ConfigFileName = "config.xml");
        public ErrorCode WriteFile(string ConfigFileName = "config.xml");
        public ref ConfigData GetConfig();
    }
}
