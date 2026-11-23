namespace Exter.Core;

public sealed class EngineStatus
{
    public bool Running { get; init; }
    public bool MicConnected { get; init; }
    public bool OutputReady { get; init; }
    public bool EffectActive { get; init; }
    public float InLevel { get; init; }      // peak 0..1
    public float OutLevel { get; init; }
    public int LastError { get; init; }
    public int Underruns { get; init; }
    public float LatencyMs { get; init; }
    public bool TransportMode { get; init; }     // output lewat driver EXTER VIRTUAL MICROPHONE
    public bool DriverAttached { get; init; }    // ring terpasang di driver
    public bool DriverStreaming { get; init; }   // ada aplikasi (Discord/game) yang sedang membaca mic virtual
    public int DriverUnderruns { get; init; }
}

/// <summary>Pembungkus aman untuk audio engine native.</summary>
public sealed class EngineController : IDisposable
{
    // DLL tidak ada / bukan x64 / versi lama tanpa fungsi yang dibutuhkan. Semua ini tidak boleh menjatuhkan UI:
    // GetStatus() dipanggil 30x/detik, jadi exception yang lolos akan membanjiri dialog error.
    private static bool NativeUnavailable(Exception e) =>
        e is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException;

    /// <summary>Mengembalikan HRESULT (0 = sukses). Memblokir sampai device siap -> panggil dari background thread.</summary>
    public int Start(string inputId, string outputId, int sampleRate, int bufferMs)
    {
        try { return Native.ex_start(inputId ?? "", outputId ?? "", sampleRate, bufferMs); }
        catch (DllNotFoundException) { return unchecked((int)0x8007007E); }       // ERROR_MOD_NOT_FOUND
        catch (BadImageFormatException) { return unchecked((int)0x8007000B); }    // ERROR_BAD_FORMAT
        catch (EntryPointNotFoundException) { return unchecked((int)0x8007007F); } // ERROR_PROC_NOT_FOUND
    }

    public void Stop()
    {
        try { Native.ex_stop(); } catch (Exception e) when (NativeUnavailable(e)) { }
    }

    public void SetActive(bool active)
    {
        try { Native.ex_set_active(active ? 1 : 0); } catch (Exception e) when (NativeUnavailable(e)) { }
    }

    public void Apply(Settings s)
    {
        var p = new Native.ExParams
        {
            InputGain = s.InputVolume / 100f,
            OutputGain = s.OutputVolume / 100f,
            HighPassHz = s.HighPassHz,
            LowPassHz = s.LowPassHz,
            GateEnabled = s.GateEnabled ? 1 : 0,
            GateThresholdDb = s.GateThresholdDb,
            CompThresholdDb = s.CompThresholdDb,
            CompRatio = s.CompRatio,
            Drive = s.Drive,
            StaticLevel = s.StaticLevel,
            BeepEnabled = s.BeepEnabled ? 1 : 0,
            BypassMode = s.OffBehavior == OffBehavior.Mute ? 1 : 0,
        };
        try { Native.ex_set_params(ref p); } catch (Exception e) when (NativeUnavailable(e)) { }
    }

    public EngineStatus GetStatus()
    {
        try
        {
            Native.ex_get_status(out var st);
            return new EngineStatus
            {
                Running = st.Running != 0, MicConnected = st.MicConnected != 0, OutputReady = st.OutputReady != 0,
                EffectActive = st.EffectActive != 0, InLevel = st.InLevel, OutLevel = st.OutLevel,
                LastError = st.LastError, Underruns = st.Underruns, LatencyMs = st.LatencyMs,
                TransportMode = st.TransportMode != 0, DriverAttached = st.DriverAttached != 0,
                DriverStreaming = st.DriverStreaming != 0, DriverUnderruns = st.DriverUnderruns,
            };
        }
        catch (Exception e) when (NativeUnavailable(e)) { return new EngineStatus(); }
    }

    public void Dispose() => Stop();
}
