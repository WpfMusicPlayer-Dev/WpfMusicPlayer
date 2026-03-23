using System.Xml.Serialization;

namespace WpfMusicPlayer.Services
{
    [XmlRoot("settings")]
    public class ConfigData
    {
        public enum ThemeMode
        {
            [XmlEnum("light")]
            Light,
            [XmlEnum("dark")]
            Dark,
            [XmlEnum("system")]
            System
        }

        [XmlElement("theme")]
        public ThemeMode Theme { get; set; }

        public enum BackgroundMode
        {
            [XmlEnum("solid")]
            Solid,
            [XmlEnum("acrylic")]
            Acrylic,
            [XmlEnum("image-blur")]
            ImageBlur
        }

        [XmlElement("background")]
        public BackgroundMode Background { get; set; }
    }
}
