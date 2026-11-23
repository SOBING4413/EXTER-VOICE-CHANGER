using System.Runtime.InteropServices;

namespace Exter.Core;

/// <summary>P/Invoke ke Exter.AudioEngine.dll. Layout struct HARUS sama dengan AudioEngine.h.</summary>
internal static class Native
{
    private const string Dll = "Exter.AudioEngine.dll";

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct ExDevice
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Name;
        public int IsDefault;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ExParams
    {
        public float InputGain;
        public float OutputGain;
        public float HighPassHz;
        public float LowPassHz;
        public int GateEnabled;
        public float GateThresholdDb;
        public float CompThresholdDb;
        public float CompRatio;
        public float Drive;
        public float StaticLevel;
        public int BeepEnabled;
        public int BypassMode;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ExStatus
    {
        public int Running;
        public int MicConnected;
        public int OutputReady;
        public int EffectActive;
        public float InLevel;
        public float OutLevel;
        public int LastError;
        public int Underruns;
        public float LatencyMs;
        // Ditambahkan untuk EXTER VIRTUAL MICROPHONE (urutan HARUS sama dengan AudioEngine.h)
        public int TransportMode;
        public int DriverAttached;
        public int DriverStreaming;
        public int DriverUnderruns;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int ex_enumerate_devices(int flow, [Out] ExDevice[] devices, int maxCount);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern int ex_start(string captureId, string renderId, int sampleRate, int bufferMs);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void ex_stop();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void ex_set_params(ref ExParams p);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void ex_set_active(int active);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void ex_get_status(out ExStatus s);
}
