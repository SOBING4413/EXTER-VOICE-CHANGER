namespace Exter.Core;

public sealed record AudioDeviceInfo(string Id, string Name, bool IsDefault)
{
    public override string ToString() => IsDefault ? Name + "  (Default)" : Name;
}

/// <summary>Mendeteksi perangkat audio Windows (Windows Audio Session API / MMDevice) lewat engine native.</summary>
public static class DeviceManager
{
    public static IReadOnlyList<AudioDeviceInfo> GetInputDevices() => Enumerate(1);
    public static IReadOnlyList<AudioDeviceInfo> GetOutputDevices() => Enumerate(0);

    private static IReadOnlyList<AudioDeviceInfo> Enumerate(int flow)
    {
        try
        {
            var buf = new Native.ExDevice[64];
            int n = Native.ex_enumerate_devices(flow, buf, buf.Length);
            var list = new List<AudioDeviceInfo>(n);
            for (int i = 0; i < n; i++)
                list.Add(new AudioDeviceInfo(buf[i].Id, buf[i].Name, buf[i].IsDefault != 0));
            return list;
        }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException)
        {
            return Array.Empty<AudioDeviceInfo>();
        }
    }
}
