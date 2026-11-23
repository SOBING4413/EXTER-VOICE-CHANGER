# Membangun Exter (Release x64) dan menyalin hasilnya ke folder dist\ - jalankan dari PowerShell.
[CmdletBinding()]
param([switch]$WithDriver)   # -WithDriver: ikut membangun driver (butuh WDK) dan menyertakannya di installer
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio 2022 tidak ditemukan. Install workload '.NET desktop development' dan 'Desktop development with C++'." }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild tidak ditemukan." }

& $msbuild "$root\Exter.sln" -restore -m -p:Configuration=Release -p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Build gagal." }

$out = Join-Path $root 'Exter.App\bin\Release\net8.0-windows'
$dist = Join-Path $root 'dist\ExterVoiceChanger'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
Copy-Item "$out\*" $dist -Recurse
# Skrip installer driver dipasang di installer\ (tempat VirtualAudioManager.TryLaunchInstaller mencarinya).
$inst = Join-Path $dist 'installer'
New-Item -ItemType Directory -Path $inst | Out-Null
Copy-Item "$PSScriptRoot\Install-ExterVirtualMic.ps1" $inst
if ($WithDriver) {
    & "$root\Exter.Driver\Build-Driver.ps1" -Configuration Release
    Copy-Item "$root\dist\driver" (Join-Path $inst 'driver') -Recurse
} else {
    Write-Host "Catatan: driver TIDAK disertakan (jalankan dengan -WithDriver, butuh WDK). Tombol INSTALL V-MIC akan meminta paket driver." -ForegroundColor Yellow
}
Write-Host "Selesai: $dist\ExterVoiceChanger.exe" -ForegroundColor Green
