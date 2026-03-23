using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Controls.Primitives;
using System.Xml;
using System.Xml.Linq;
using System.Xml.Serialization;
using static WpfMusicPlayer.Services.ConfigData;

namespace WpfMusicPlayer.Services
{
    public class ConfigRender
    {
        private static readonly Lazy<ConfigRender> render = new Lazy<ConfigRender>(() => new ConfigRender());
        public static ConfigRender Render => render.Value;

        private ConfigRender(string ConfigFileName = "config.xml") => Reload(ConfigFileName);
        ~ConfigRender()
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
        private ErrorCode InternalCreateConfigFile(string ConfigFilePath)
        {
            ConfigData = new ConfigData { Theme = ThemeMode.System, Background = BackgroundMode.Acrylic };

            return InternalWriteFile(ConfigFilePath);
        }
        public ErrorCode CreateConfigFile(string ConfigFileName = "config.xml")
        {
            try
            {
                var ConfigFilePath = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (ConfigFilePath == null)
                    return ErrorCode.PermissionDenied;

                var ConfigFile = Path.Combine(ConfigFilePath, ConfigFileName);

                return InternalCreateConfigFile(ConfigFile);
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
                var ConfigFilePath = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (ConfigFilePath == null)
                    return ErrorCode.PermissionDenied;

                var ConfigFile = Path.Combine(ConfigFilePath, ConfigFileName);
                if (!File.Exists(ConfigFile))
                    return InternalCreateConfigFile(ConfigFile);

                try
                {
                    using var File = new FileStream(ConfigFile, FileMode.Open, FileAccess.Read);

                    try
                    {
                        ConfigData = (ConfigData)(new XmlSerializer(typeof(ConfigData)).Deserialize(File));
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

        private ErrorCode InternalWriteFile(string ConfigFilePath)
        {
            try
            {
                using var File = new FileStream(ConfigFilePath, FileMode.Create, FileAccess.Write);
                new XmlSerializer(typeof(ConfigData)).Serialize(File, ConfigData);
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
        private ErrorCode WriteFile(string ConfigFileName = "config.xml")
        {
            try
            {
                var ConfigFilePath = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (ConfigFilePath == null)
                    return ErrorCode.PermissionDenied;

                var ConfigFile = Path.Combine(ConfigFilePath, ConfigFileName);

                return InternalWriteFile(ConfigFile);
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
            return ref ConfigData;
        }

        private static ConfigData ConfigData = new ConfigData();
    }
}
