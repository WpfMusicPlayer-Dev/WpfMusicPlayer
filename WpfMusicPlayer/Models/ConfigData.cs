// SPDX-License-Identifier: MIT
using System.Xml.Serialization;

namespace WpfMusicPlayer.Models
{
    [XmlRoot("settings")]
    public class ConfigData
    {
        [XmlElement("audio-settings")]
        public AudioSettings Audio { get; set; } = new();

        public class AudioSettings
        {
            public enum BackendType
            {
                [XmlEnum("faudio")] FAudio = 0,
#if WINDOWS
                [XmlEnum("wasapi-exclusive")] WasapiExclusive = 1
#endif
            }

            [XmlElement("backend")] public BackendType Backend { get; set; } = BackendType.FAudio;
            [XmlElement("output-device-id")] public string OutputDeviceId { get; set; } = string.Empty;
            [XmlElement("sample-rate")] public int SampleRate { get; set; } = 44100;

            public enum ChannelType
            {
                [XmlEnum("system")] System = 0,
                [XmlEnum("mono")] Mono = 1,
                [XmlEnum("stereo")] Stereo = 2,
                [XmlEnum("surround_5_1")] Surround51 = 3,
                [XmlEnum("surround_7_1")] Surround71 = 4
            }

            public enum BitDepthType
            {
                [XmlEnum("system")] System = 0,
                [XmlEnum("16bit")] Bit16 = 16,
                [XmlEnum("24bit")] Bit24 = 24,
                [XmlEnum("32bit")] Bit32 = 32
            }

            [XmlElement("channel-type")] public ChannelType Channel { get; set; } = ChannelType.System;
            [XmlElement("bit-depth")] public BitDepthType BitDepth { get; set; } = BitDepthType.System;
            [XmlElement("volume")] public double Volume { get; set; } = 1.0;
        }
        
        [XmlElement("ui-settings")]
        public UISettings UI { get; set; } = new();

        public class UISettings
        {
            public enum ThemeMode
            {
                [XmlEnum("light")] Light,
                [XmlEnum("dark")] Dark,
                [XmlEnum("system")] System
            }

            [XmlElement("theme")] public ThemeMode Theme { get; set; }

            public enum BackgroundMode
            {
                [XmlEnum("solid")] Solid,
                [XmlEnum("acrylic")] Acrylic,
                [XmlEnum("image-blur")] ImageBlur
            }

            [XmlElement("background")]
            public BackgroundMode Background { get; set; }
        }

        [XmlElement("desktop-lyric")]
        public DesktopLyricSettings DesktopLyric { get; set; } = new();

        public class DesktopLyricSettings
        {
            [XmlElement("desktop-lyric-enabled")]
            public bool IsDesktopLyricEnabled { get; set; }
            
            [XmlElement("desktop-lyric-font-size")]
            public double DesktopLyricFontSize { get; set; } = 24;

            [XmlElement("desktop-lyric-aux-customizable")]
            public bool IsDesktopLyricAuxCustomizable { get; set; }

            [XmlElement("desktop-lyric-aux-font-size")]
            public double DesktopLyricAuxFontSize { get; set; } = 18;
        }
    }
}
