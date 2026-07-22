// SPDX-License-Identifier: MIT

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
        private readonly IAudioOutputFormatProvider _audioOutputFormatProvider;
        private readonly string _configFileName;

        public ConfigProvider(IAudioOutputFormatProvider audioOutputFormatProvider)
            : this(audioOutputFormatProvider, "config.xml")
        {
        }

        public ConfigProvider(
            IAudioOutputFormatProvider audioOutputFormatProvider,
            string configFileName)
        {
            _audioOutputFormatProvider = audioOutputFormatProvider;
            _configFileName = configFileName;
            if (Reload(configFileName) != ErrorCode.NoError)
            {
                EnsureConfigSections();
                MigrateLegacySystemAudioSettings();
            }
        }

        ~ConfigProvider()
        {
            WriteFile(_configFileName);
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
            _configData = CreateDefaultConfigData();

            return InternalWriteFile(configFilePath);
        }

        private ConfigData CreateDefaultConfigData()
        {
            var systemFormat = _audioOutputFormatProvider.GetSystemDefaultOutputFormat();
            return new ConfigData
            {
                UI = new UISettings
                {
                    Background = UISettings.BackgroundMode.ImageBlur,
                    Theme = UISettings.ThemeMode.System
                },
                Audio = new AudioSettings
                {
                    Backend = AudioSettings.BackendType.FAudio,
                    OutputDeviceId = string.Empty,
                    Channel = systemFormat.Channel,
                    BitDepth = systemFormat.BitDepth,
                    SampleRate = systemFormat.SampleRate
                }
            };
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
                        EnsureConfigSections();
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

                if (MigrateLegacySystemAudioSettings())
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

            return ErrorCode.NoError;
        }

        private bool MigrateLegacySystemAudioSettings()
        {
            var audio = _configData.Audio ??= new AudioSettings();
            var migrateChannel = audio.Channel == AudioSettings.ChannelType.System;
            var migrateBitDepth = audio.BitDepth == AudioSettings.BitDepthType.System;
            var migrateSampleRate = audio.SampleRate <= 0;
            if (!migrateChannel && !migrateBitDepth && !migrateSampleRate)
                return false;

            var systemFormat = _audioOutputFormatProvider.GetSystemDefaultOutputFormat();
            if (migrateChannel)
                audio.Channel = systemFormat.Channel;
            if (migrateBitDepth)
                audio.BitDepth = systemFormat.BitDepth;
            if (migrateSampleRate)
                audio.SampleRate = systemFormat.SampleRate;
            return true;
        }

        private void EnsureConfigSections()
        {
            _configData.Audio ??= new AudioSettings();
            _configData.UI ??= new UISettings();
            _configData.DesktopLyric ??= new DesktopLyricSettings();
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

        private ConfigData _configData = new ConfigData();
    }
}
