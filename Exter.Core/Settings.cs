using System.Xml.Serialization;

namespace Exter.Core;

public enum PttMode { PushToTalk, AlwaysOn, AlwaysOff }

/// <summary>Perilaku audio ketika efek radio tidak aktif.</summary>
public enum OffBehavior { Bypass, Mute }

/// <summary>Konfigurasi aplikasi, disimpan sebagai XML di %AppData%\Exter\settings.xml.</summary>
[XmlRoot("ExterSettings")]
public sealed class Settings
{
    public const int DefaultPttKey = 0x56;   // virtual-key 'V'

    public bool PowerOn { get; set; } = true;
    public PttMode Mode { get; set; } = PttMode.PushToTalk;
    public int PttKey { get; set; } = DefaultPttKey;
    public OffBehavior OffBehavior { get; set; } = OffBehavior.Bypass;

    public string InputDeviceId { get; set; } = "";    // kosong = default Windows
    public string OutputDeviceId { get; set; } = "";   // kosong = auto-pilih virtual mic
    public int SampleRate { get; set; } = 48000;
    public int BufferMs { get; set; } = 40;
    public int InputVolume { get; set; } = 100;        // persen, 0..200
    public int OutputVolume { get; set; } = 100;       // persen, 0..200

    public bool GateEnabled { get; set; } = true;
    public float GateThresholdDb { get; set; } = -45f;
    public float HighPassHz { get; set; } = 300f;
    public float LowPassHz { get; set; } = 3400f;
    public float CompThresholdDb { get; set; } = -22f;
    public float CompRatio { get; set; } = 4f;
    public float Drive { get; set; } = 1.8f;
    public float StaticLevel { get; set; } = 0.25f;     // 0..1
    public bool BeepEnabled { get; set; } = true;

    [XmlIgnore]
    public static string FilePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Exter", "settings.xml");

    private static readonly XmlSerializer Serializer = new(typeof(Settings));

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath)) return Read(FilePath);
            string bundled = Path.Combine(AppContext.BaseDirectory, "settings.xml");   // default dari Exter.Config
            if (File.Exists(bundled)) return Read(bundled);
        }
        catch { /* file rusak -> pakai default */ }
        return new Settings();
    }

    private static Settings Read(string path)
    {
        using var fs = File.OpenRead(path);
        var s = (Settings?)Serializer.Deserialize(fs) ?? new Settings();
        s.Clamp();
        return s;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            string tmp = FilePath + ".tmp";
            using (var fs = File.Create(tmp)) Serializer.Serialize(fs, this);
            File.Move(tmp, FilePath, true);
        }
        catch { /* gagal simpan tidak boleh menjatuhkan aplikasi */ }
    }

    public void Clamp()
    {
        InputVolume = Math.Clamp(InputVolume, 0, 200);
        OutputVolume = Math.Clamp(OutputVolume, 0, 200);
        SampleRate = SampleRate == 44100 ? 44100 : 48000;
        BufferMs = Math.Clamp(BufferMs, 10, 200);
        GateThresholdDb = Math.Clamp(GateThresholdDb, -80f, -10f);
        HighPassHz = Math.Clamp(HighPassHz, 80f, 1200f);
        LowPassHz = Math.Clamp(LowPassHz, 1500f, 8000f);
        CompThresholdDb = Math.Clamp(CompThresholdDb, -50f, -5f);
        CompRatio = Math.Clamp(CompRatio, 1f, 20f);
        Drive = Math.Clamp(Drive, 1f, 8f);
        StaticLevel = Math.Clamp(StaticLevel, 0f, 1f);
        if (PttKey <= 0 || PttKey > 255) PttKey = DefaultPttKey;
    }
}
