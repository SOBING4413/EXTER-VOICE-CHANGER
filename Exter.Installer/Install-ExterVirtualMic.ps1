<#
.SYNOPSIS
    Memasang / mencopot driver EXTER VIRTUAL MICROPHONE (Exter Virtual Audio Device).

.DESCRIPTION
    Driver ini adalah driver root-enumerated (hardware ID Root\ExterVirtualAudio): tidak ada perangkat fisik yang
    memicu pemasangannya, jadi skrip ini membuat device node-nya (seperti "devcon install") lalu memasang paket driver.

    Perlu Administrator (skrip meminta UAC sendiri). Paket driver (ExterVirtualAudio.inf/.sys/.cat) dicari di:
    -DriverDir, folder "driver" di samping skrip ini, folder skrip, lalu output build di Exter.Driver\.

    PENTING - penandatanganan:
      * Paket bertanda tangan Microsoft (attestation/WHQL): langsung terpasang di PC mana pun.
      * Paket bertanda tangan SERTIFIKAT TEST (hasil build Visual Studio biasa): Windows menolaknya kecuali
        test-signing aktif (bcdedit /set testsigning on, Secure Boot harus mati, lalu restart). Pakai -TrustSigner
        agar sertifikat test itu dipercaya di PC ini. Jangan dipakai untuk sertifikat yang tidak Anda buat sendiri.

.PARAMETER DriverDir    Folder paket driver (opsional).
.PARAMETER Uninstall    Copot device node dan paket driver.
.PARAMETER TrustSigner  Impor sertifikat penanda tangan paket ke Trusted Root + Trusted Publishers (LocalMachine).
.PARAMETER NoPause      Jangan menunggu Enter di akhir.
#>
[CmdletBinding()]
param(
    [string]$DriverDir,
    [switch]$Uninstall,
    [switch]$TrustSigner,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$HardwareId = 'Root\ExterVirtualAudio'
$InfName    = 'ExterVirtualAudio.inf'

function Wait-Close { if (-not $NoPause) { [void](Read-Host 'Tekan Enter untuk menutup') } }
function Fail($msg) { Write-Host "GAGAL: $msg" -ForegroundColor Red; Wait-Close; exit 1 }

# ---- Administrator ------------------------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($DriverDir)    { $argList += @('-DriverDir', "`"$DriverDir`"") }
    if ($Uninstall)    { $argList += '-Uninstall' }
    if ($TrustSigner)  { $argList += '-TrustSigner' }
    if ($NoPause)      { $argList += '-NoPause' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit 0
}

# ---- P/Invoke SetupAPI (setara devcon install / remove) -------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class ExterSetup
{
    static readonly Guid MediaClass = new Guid("4d36e96c-e325-11ce-bfc1-08002be10318");
    const int DICD_GENERATE_ID = 1, SPDRP_HARDWAREID = 1, DIF_REMOVE = 0x05, DIF_REGISTERDEVICE = 0x19, INSTALLFLAG_FORCE = 1;
    static readonly IntPtr InvalidHandle = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }

    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid cls, IntPtr hwnd);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid cls, string desc, IntPtr hwnd, int flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, int prop, byte[] buf, int size);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, int prop, IntPtr type, byte[] buf, int size, IntPtr required);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(int fn, IntPtr set, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(ref Guid cls, string enumerator, IntPtr hwnd, int flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, int index, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, int flags, out bool reboot);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string path, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    static SP_DEVINFO_DATA NewData()
    {
        SP_DEVINFO_DATA d = new SP_DEVINFO_DATA();
        d.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        return d;
    }

    // Mengembalikan 0 bila sukses, selain itu kode error Win32. reboot=true bila perlu restart.
    public static int Install(string infPath, string hwid, bool createNode, out bool reboot)
    {
        reboot = false;
        if (createNode)
        {
            Guid cls = MediaClass;
            IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
            if (set == InvalidHandle) return Marshal.GetLastWin32Error();
            try
            {
                SP_DEVINFO_DATA data = NewData();
                if (!SetupDiCreateDeviceInfoW(set, "MEDIA", ref cls, null, IntPtr.Zero, DICD_GENERATE_ID, ref data))
                    return Marshal.GetLastWin32Error();
                byte[] hw = Encoding.Unicode.GetBytes(hwid + "\0\0");   // REG_MULTI_SZ
                if (!SetupDiSetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, hw, hw.Length))
                    return Marshal.GetLastWin32Error();
                if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data))
                    return Marshal.GetLastWin32Error();
            }
            finally { SetupDiDestroyDeviceInfoList(set); }
        }
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hwid, infPath, INSTALLFLAG_FORCE, out reboot))
            return Marshal.GetLastWin32Error();
        return 0;
    }

    // Jumlah device node yang dicopot, atau -1 bila gagal membuka daftar perangkat.
    public static int RemoveDevices(string hwid)
    {
        Guid cls = MediaClass;
        IntPtr set = SetupDiGetClassDevsW(ref cls, null, IntPtr.Zero, 0);   // semua perangkat kelas MEDIA (termasuk tidak hadir)
        if (set == InvalidHandle) return -1;
        int removed = 0;
        try
        {
            for (int i = 0; ; i++)
            {
                SP_DEVINFO_DATA d = NewData();
                if (!SetupDiEnumDeviceInfo(set, i, ref d)) break;
                byte[] buf = new byte[4096];
                if (!SetupDiGetDeviceRegistryPropertyW(set, ref d, SPDRP_HARDWAREID, IntPtr.Zero, buf, buf.Length, IntPtr.Zero)) continue;
                foreach (string s in Encoding.Unicode.GetString(buf).Split('\0'))
                {
                    if (string.Equals(s, hwid, StringComparison.OrdinalIgnoreCase))
                    {
                        if (SetupDiCallClassInstaller(DIF_REMOVE, set, ref d)) removed++;
                        break;
                    }
                }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return removed;
    }

    // true bila \\.\ExterVirtualMic bisa dibuka (driver hidup dan control device terpasang).
    public static bool ProbeControlDevice()
    {
        IntPtr h = CreateFileW(@"\\.\ExterVirtualMic", 0xC0000000u, 3u, IntPtr.Zero, 3u, 0u, IntPtr.Zero);
        if (h == InvalidHandle) return false;
        CloseHandle(h);
        return true;
    }
}
'@

function Get-ExterDevices { Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $HardwareId } }

# ---- Uninstall ------------------------------------------------------------------------------------
if ($Uninstall) {
    Write-Host 'Mencopot EXTER VIRTUAL MICROPHONE ...' -ForegroundColor Cyan
    $n = [ExterSetup]::RemoveDevices($HardwareId)
    if ($n -lt 0) { Fail 'Tidak bisa membaca daftar perangkat.' }
    Write-Host "Device node dicopot: $n"
    try {
        Get-WindowsDriver -Online -All | Where-Object { $_.OriginalFileName -like "*\$InfName" } | ForEach-Object {
            Write-Host "Menghapus paket driver $($_.Driver) dari driver store"
            & pnputil.exe /delete-driver $_.Driver /uninstall /force | Out-Null
        }
    } catch { Write-Host "Peringatan: tidak bisa menghapus paket dari driver store: $($_.Exception.Message)" -ForegroundColor Yellow }
    Write-Host 'Selesai. Restart Windows bila Discord/game masih menampilkan perangkatnya.' -ForegroundColor Green
    Wait-Close; exit 0
}

# ---- Cari paket driver ----------------------------------------------------------------------------
$candidates = @()
if ($DriverDir) { $candidates += $DriverDir }
$candidates += (Join-Path $PSScriptRoot 'driver'), $PSScriptRoot
$buildRoot = Join-Path $PSScriptRoot '..\Exter.Driver\sysvad\ExterAudio'
if (Test-Path $buildRoot) {
    $candidates += (Get-ChildItem $buildRoot -Recurse -Filter $InfName -ErrorAction SilentlyContinue |
                    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object LastWriteTime -Descending |
                    ForEach-Object { $_.DirectoryName })
}
$pkg = $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ $InfName)) } | Select-Object -First 1
if (-not $pkg) { Fail "Paket driver ($InfName) tidak ditemukan. Bangun dulu: Exter.Driver\Build-Driver.ps1, atau berikan -DriverDir." }
$pkg = (Resolve-Path $pkg).Path
$inf = Join-Path $pkg $InfName
foreach ($f in 'ExterVirtualAudio.sys', 'ExterVirtualAudio.cat') {
    if (-not (Test-Path (Join-Path $pkg $f))) { Fail "$f tidak ada di $pkg (paket driver tidak lengkap; build gagal membuat katalog?)." }
}
Write-Host "Paket driver: $pkg" -ForegroundColor Cyan

# ---- Tanda tangan ---------------------------------------------------------------------------------
$sig = Get-AuthenticodeSignature (Join-Path $pkg 'ExterVirtualAudio.cat')
if ($sig.Status -eq 'Valid') {
    Write-Host "Tanda tangan valid: $($sig.SignerCertificate.Subject)" -ForegroundColor Green
} else {
    Write-Host "Tanda tangan paket: $($sig.Status) (bukan tanda tangan Microsoft/CA tepercaya)." -ForegroundColor Yellow
    $bcd = (& bcdedit.exe /enum '{current}' 2>&1) -join "`n"
    $testOn = $bcd -match '(?im)^\s*testsigning\s+Yes'
    if ($TrustSigner) {
        if (-not $sig.SignerCertificate) { Fail 'Paket tidak bertanda tangan sama sekali; tidak ada sertifikat untuk dipercaya.' }
        Write-Host "Mempercayai sertifikat: $($sig.SignerCertificate.Subject)" -ForegroundColor Yellow
        foreach ($storeName in 'Root', 'TrustedPublisher') {
            $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
            $store.Open('ReadWrite'); $store.Add($sig.SignerCertificate); $store.Close()
        }
    }
    if (-not $testOn) {
        Fail ("Windows tidak akan memuat driver bertanda tangan test saat test-signing mati.`n" +
              "  1) Matikan Secure Boot di UEFI (bila aktif)`n" +
              "  2) bcdedit /set testsigning on   (Administrator)`n" +
              "  3) Restart, lalu jalankan skrip ini lagi dengan -TrustSigner`n" +
              "  Untuk dipakai di PC lain tanpa test-signing, paket harus ditandatangani Microsoft (attestation signing).")
    }
    Write-Host 'Test-signing aktif.' -ForegroundColor Green
}

# ---- Pasang ---------------------------------------------------------------------------------------
Write-Host 'Menambahkan paket ke driver store ...'
& pnputil.exe /add-driver $inf | Out-Host
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { Fail "pnputil /add-driver gagal (kode $LASTEXITCODE)." }

$existing = @(Get-ExterDevices)
$reboot = $false
Write-Host ($(if ($existing.Count) { 'Memperbarui driver pada perangkat yang sudah ada ...' } else { 'Membuat perangkat Exter Virtual Audio Device ...' }))
$err = [ExterSetup]::Install($inf, $HardwareId, ($existing.Count -eq 0), [ref]$reboot)
if ($err -ne 0) {
    $msg = (New-Object System.ComponentModel.Win32Exception($err)).Message
    Fail ("Pemasangan gagal (Win32 $err): $msg`n  Periksa C:\Windows\INF\setupapi.dev.log untuk detail.")
}

# ---- Verifikasi -----------------------------------------------------------------------------------
Write-Host 'Memverifikasi ...'
$ok = $false; $endpoint = $null
for ($i = 0; $i -lt 30 -and -not $ok; $i++) {
    Start-Sleep -Milliseconds 500
    $dev = Get-ExterDevices | Select-Object -First 1
    $endpoint = Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*EXTER VIRTUAL MICROPHONE*' } | Select-Object -First 1
    $ok = $dev -and $dev.Status -eq 'OK' -and $endpoint -and [ExterSetup]::ProbeControlDevice()
}
if ($ok) {
    Write-Host "OK  : perangkat   = $($dev.FriendlyName) ($($dev.Status))" -ForegroundColor Green
    Write-Host "OK  : endpoint    = $($endpoint.FriendlyName)" -ForegroundColor Green
    Write-Host 'OK  : \\.\ExterVirtualMic dapat dibuka' -ForegroundColor Green
    Write-Host "`nSelesai. Di Discord/game pilih 'EXTER VIRTUAL MICROPHONE' sebagai Input Device." -ForegroundColor Green
    if ($reboot) { Write-Host 'Windows meminta restart untuk menyelesaikan pemasangan.' -ForegroundColor Yellow }
    Wait-Close; exit 0
}
$dev = Get-ExterDevices | Select-Object -First 1
Write-Host ("Status perangkat: " + $(if ($dev) { "$($dev.Status) / problem=$($dev.Problem)" } else { 'tidak ada' })) -ForegroundColor Yellow
Write-Host ("Endpoint audio  : " + $(if ($endpoint) { $endpoint.FriendlyName } else { 'belum muncul' })) -ForegroundColor Yellow
Write-Host ("Control device  : " + $(if ([ExterSetup]::ProbeControlDevice()) { 'dapat dibuka' } else { 'tidak dapat dibuka' })) -ForegroundColor Yellow
if ($reboot) { Write-Host 'Restart Windows lalu cek lagi.' -ForegroundColor Yellow }
Fail 'Driver terpasang tetapi belum lolos verifikasi. Lihat C:\Windows\INF\setupapi.dev.log dan Device Manager (Sound, video and game controllers).'
