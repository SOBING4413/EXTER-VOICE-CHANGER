using Exter.Core;

namespace Exter.VirtualAudio;

/// <summary>
/// Integrasi EXTER VIRTUAL MICROPHONE (driver audio Exter sendiri). Tidak ada ketergantungan pada virtual cable
/// pihak ketiga (VB-Audio, VoiceMeeter, dsb.).
///
///   Mic fisik -> WASAPI -> Exter.AudioEngine (DSP radio) -> shared-memory ring -> driver Exter
///             -> endpoint capture "EXTER VIRTUAL MICROPHONE" -> Windows Audio Engine -> Discord / game
///
/// "Output" yang dipilih di Exter bukan perangkat WASAPI melainkan entri khusus <see cref="VirtualOutput"/>,
/// yang membuat engine memakai transport ke driver (lihat EX_VIRTUAL_MIC_ID di AudioEngine.h).
/// </summary>
public static class VirtualAudioManager
{
    /// <summary>Harus sama dengan EX_VIRTUAL_MIC_ID di Exter.AudioEngine/AudioEngine.h.</summary>
    public const string VirtualOutputId = "exter:virtual-mic";

    /// <summary>Nama endpoint capture yang terdaftar di Windows oleh driver (juga tampil di Discord).</summary>
    public const string VirtualMicName = "EXTER VIRTUAL MICROPHONE";

    public static readonly AudioDeviceInfo VirtualOutput =
        new(VirtualOutputId, VirtualMicName + "  (Exter driver)", false);

    // HRESULT dari ex_start / transport
    public const int ErrDriverNotInstalled = unchecked((int)0x80070002);   // ERROR_FILE_NOT_FOUND saat membuka \\.\ExterVirtualMic
    public const int ErrAccessDenied       = unchecked((int)0x80070005);
    public const int ErrAbiMismatch        = unchecked((int)0x8007051A);   // HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH=1306); driver mengembalikan STATUS_REVISION_MISMATCH
    public const int ErrLoopback           = unchecked((int)0x8EE70001);   // mic fisik = EXTER VIRTUAL MICROPHONE
    public const int ErrDllNotFound        = unchecked((int)0x8007007E);   // ERROR_MOD_NOT_FOUND
    public const int ErrBadImage           = unchecked((int)0x8007000B);   // ERROR_BAD_FORMAT (DLL bukan x64)
    public const int ErrEntryPoint         = unchecked((int)0x8007007F);   // ERROR_PROC_NOT_FOUND (DLL versi lama)

    public static bool IsVirtualOutput(AudioDeviceInfo? d) => d != null && d.Id == VirtualOutputId;

    /// <summary>Endpoint capture "EXTER VIRTUAL MICROPHONE" yang dilihat Windows (bukti driver terdaftar).</summary>
    public static AudioDeviceInfo? FindVirtualCaptureDevice(IEnumerable<AudioDeviceInfo> inputs) =>
        inputs.FirstOrDefault(IsExterVirtualMicName);

    /// <summary>True bila Windows mendaftarkan EXTER VIRTUAL MICROPHONE sebagai perangkat input.</summary>
    public static bool IsDriverInstalled() => FindVirtualCaptureDevice(DeviceManager.GetInputDevices()) != null;

    /// <summary>Mic fisik tidak boleh berupa EXTER VIRTUAL MICROPHONE sendiri: itu feedback loop.</summary>
    public static bool IsLoopbackRisk(AudioDeviceInfo? input) => input != null && IsExterVirtualMicName(input);

    private static bool IsExterVirtualMicName(AudioDeviceInfo d) =>
        d.Name.Contains(VirtualMicName, StringComparison.OrdinalIgnoreCase);

    public static string DescribeError(int hr) => hr switch
    {
        0 => "",
        ErrDriverNotInstalled => "EXTER VIRTUAL MICROPHONE DRIVER NOT INSTALLED - click INSTALL V-MIC",
        ErrAccessDenied => "ACCESS TO EXTER DRIVER DENIED - run the installer again / check driver security settings",
        ErrAbiMismatch => "EXTER DRIVER VERSION MISMATCH - reinstall the driver from this build",
        ErrLoopback => "INPUT IS EXTER VIRTUAL MICROPHONE - WOULD CAUSE FEEDBACK. SELECT YOUR REAL MICROPHONE.",
        ErrDllNotFound => "Exter.AudioEngine.dll NOT FOUND - BUILD THE FULL SOLUTION (x64)",
        ErrBadImage => "Exter.AudioEngine.dll IS NOT x64 - REBUILD THE SOLUTION WITH PLATFORM x64",
        ErrEntryPoint => "Exter.AudioEngine.dll IS OUT OF DATE - REBUILD THE FULL SOLUTION",
        _ => $"ENGINE ERROR 0x{hr:X8} - RETRYING",
    };

    public static string GetInstallInstructions() =>
        "1. Close Exter.\n" +
        "2. Run Exter.Installer\\Install-ExterVirtualMic.ps1 as Administrator (or click INSTALL V-MIC).\n" +
        "3. If Windows is in test-signing mode for development builds, restart when asked.\n" +
        "4. Open Exter: VIRTUAL MIC shows READY.\n" +
        "5. In Discord: Settings > Voice & Video > Input Device = \"" + VirtualMicName + "\".";

    /// <summary>
    /// Menjalankan installer driver Exter (PowerShell, UAC admin). Mengembalikan false + pesan bila
    /// skrip installer tidak ditemukan atau pengguna menolak UAC.
    /// </summary>
    public static bool TryLaunchInstaller(out string message)
    {
        string[] roots =
        {
            Path.Combine(AppContext.BaseDirectory, "installer"),
            AppContext.BaseDirectory,
            Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "Exter.Installer")),
        };
        foreach (var root in roots)
        {
            string script = Path.Combine(root, "Install-ExterVirtualMic.ps1");
            if (!File.Exists(script)) continue;
            try
            {
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("powershell.exe",
                    $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\"")
                { UseShellExecute = true, Verb = "runas", WorkingDirectory = root });
                message = "";
                return true;
            }
            catch (System.ComponentModel.Win32Exception) { message = "Installer cancelled (UAC)."; return false; }
        }
        message = "Install-ExterVirtualMic.ps1 not found next to the application.\n\n" + GetInstallInstructions();
        return false;
    }
}
