using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Exter.Core;

/// <summary>
/// Global keyboard hook (WH_KEYBOARD_LL): menangkap key-down DAN key-up walau aplikasi tidak di foreground.
/// Harus di-Install() dari thread yang punya message loop (UI thread WPF). Tidak memblokir/menelan tombol.
/// </summary>
public sealed class HotkeyManager : IDisposable
{
    private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct KBDLLHOOKSTRUCT { public uint vkCode, scanCode, flags, time; public IntPtr dwExtraInfo; }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);
    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);

    private const int WH_KEYBOARD_LL = 13;
    private const int WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105;

    private IntPtr _hook = IntPtr.Zero;
    private HookProc? _proc;                 // simpan referensi agar tidak di-GC
    private Action<int>? _capture;
    private int _targetKey;

    public int TargetKey
    {
        get => _targetKey;
        set { _targetKey = value; IsDown = false; }
    }

    public bool IsDown { get; private set; }

    /// <summary>(virtualKey, isDown) - hanya untuk TargetKey. Dipanggil di thread pemasang hook.</summary>
    public event Action<int, bool>? KeyChanged;

    public void Install()
    {
        if (_hook != IntPtr.Zero) return;
        _proc = Callback;
        _hook = SetWindowsHookEx(WH_KEYBOARD_LL, _proc, GetModuleHandle(null), 0);
        if (_hook == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    /// <summary>
    /// Pengaman PTT macet: bila key-up terlewat (mis. Win+L / UAC / desktop aman saat tombol ditahan, atau hook
    /// di-skip Windows karena callback terlalu lama), IsDown tetap true dan radio "ON AIR" selamanya.
    /// Dipanggil berkala dari UI thread; bila tombol ternyata sudah dilepas, kirim KeyChanged(false).
    /// </summary>
    public void Resync()
    {
        if (!IsDown || _hook == IntPtr.Zero) return;
        if ((GetAsyncKeyState(_targetKey) & 0x8000) != 0) return;
        IsDown = false;
        KeyChanged?.Invoke(_targetKey, false);
    }

    /// <summary>Tombol berikutnya yang ditekan dikirim ke callback (0 = dibatalkan dengan Esc).</summary>
    public void CaptureNext(Action<int> callback) => _capture = callback;
    public void CancelCapture() => _capture = null;

    private IntPtr Callback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            try
            {
                var k = Marshal.PtrToStructure<KBDLLHOOKSTRUCT>(lParam);
                int vk = (int)k.vkCode;
                int msg = (int)wParam;
                bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
                bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;

                if (down && _capture != null)
                {
                    var cb = _capture; _capture = null;
                    cb(vk == 0x1B ? 0 : vk);
                }
                else if (vk == _targetKey)
                {
                    if (down && !IsDown) { IsDown = true; KeyChanged?.Invoke(vk, true); }
                    else if (up && IsDown) { IsDown = false; KeyChanged?.Invoke(vk, false); }
                }
            }
            catch { /* jangan pernah melempar exception dari hook */ }
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    public void Dispose()
    {
        if (_hook != IntPtr.Zero) { UnhookWindowsHookEx(_hook); _hook = IntPtr.Zero; }
    }
}
