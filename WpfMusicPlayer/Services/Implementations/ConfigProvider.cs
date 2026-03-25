using System.IO;
using System.Reflection;
using System.Xml.Serialization;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;
using static WpfMusicPlayer.Models.ConfigData;

namespace WpfMusicPlayer.Services.Implementations
{
    public class ConfigProvider : IConfigProvider
    {
        private static readonly Lazy<ConfigProvider> _reader = new Lazy<ConfigProvider>(() => new ConfigProvider());
        public static ConfigProvider Reader => _reader.Value;

        private ConfigProvider(string configFileName = "config.xml") => Reload(configFileName);
        ~ConfigProvider()
        {
            WriteFile();
        }

        public enum ErrorCode
        {
            NoError,
            FileNoFound,
            PermissionDenied,
            FileOpenFailed,
            ConfigFileError,

            UnknownError
        }
        private ErrorCode InternalCreateConfigFile(string configFilePath)
        {
            _configData = new ConfigData
            {
                UI = new UISettings
                {
                    Background = UISettings.BackgroundMode.ImageBlur,
                    Theme = UISettings.ThemeMode.System
                },
                Audio = new AudioSettings
                {
                    Channel = AudioSettings.ChannelType.Stereo,
                    SampleRate = 48000
                }
            };

            return InternalWriteFile(configFilePath);
        }
        public ErrorCode CreateConfigFile(string configFileName = "config.xml")
        {
            try
            {
                var configFilePath = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (configFilePath == null)
                    return ErrorCode.PermissionDenied;

                var configFile = Path.Combine(configFilePath, configFileName);

                return InternalCreateConfigFile(configFile);
            }
            catch (PathTooLongException)
            {
                return ErrorCode.FileOpenFailed;
            }
            catch (Exception)
            {
                return ErrorCode.UnknownError;
            }
        }
        public ErrorCode Reload(string ConfigFileName = "config.xml")
        {
            try
            {
                var configFilePath = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (configFilePath == null)
                    return ErrorCode.PermissionDenied;

                var configFile = Path.Combine(configFilePath, ConfigFileName);
                if (!File.Exists(configFile))
                    return InternalCreateConfigFile(configFile);

                try
                {
                    using var file = new FileStream(configFile, FileMode.Open, FileAccess.Read);

                    try
                    {
                        var xmlSerializer = new XmlSerializer(typeof(ConfigData));
                        var xmlConfigData = (ConfigData?)xmlSerializer.Deserialize(file);
                        if (xmlConfigData == null)
                            return ErrorCode.ConfigFileError;

                        _configData = xmlConfigData;
                    }
                    catch (InvalidOperationException)
                    {
                        return ErrorCode.ConfigFileError;
                    }
                }
                catch (UnauthorizedAccessException)
                {
                    return ErrorCode.PermissionDenied;
                }
            }
            catch (PathTooLongException)
            {
                return ErrorCode.FileOpenFailed;
            }
            catch (Exception)
            {
                return ErrorCode.UnknownError;
            }

            return ErrorCode.NoError;
        }

        private ErrorCode InternalWriteFile(string configFilePath)
        {
            try
            {
                using var file = new FileStream(configFilePath, FileMode.Create, FileAccess.Write);
                new XmlSerializer(typeof(ConfigData)).Serialize(file, _configData);
            }
            catch (UnauthorizedAccessException)
            {
                return ErrorCode.PermissionDenied;
            }
            catch (Exception)
            {
                return ErrorCode.UnknownError;
            }

            return ErrorCode.NoError;
        }
        public ErrorCode WriteFile(string configFileName = "config.xml")
        {
            try
            {
                var configFilePath = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (configFilePath == null)
                    return ErrorCode.PermissionDenied;

                var configFile = Path.Combine(configFilePath, configFileName);

                return InternalWriteFile(configFile);
            }
            catch (PathTooLongException)
            {
                return ErrorCode.FileOpenFailed;
            }
            catch (Exception)
            {
                return ErrorCode.UnknownError;
            }
        }

        public ref ConfigData GetConfig()
        {
            return ref _configData;
        }

        private static ConfigData _configData = new ConfigData();
    }
}
