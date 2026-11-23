<#
.SYNOPSIS  Membangun driver Exter Virtual Audio Device (x64) dan menyalin paketnya ke dist\driver.
.NOTES     Butuh Visual Studio 2022 + Windows Driver Kit (WDK) yang cocok dengan Windows SDK-nya.
           Konfigurasi Debug menandatangani paket dengan sertifikat TEST (WDKTestCert) otomatis dari Visual Studio.
#>
[CmdletBinding()]
param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio 2022 tidak ditemukan." }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild tidak ditemukan." }

& $msbuild "$PSScriptRoot\Exter.Driver.sln" -m -p:Configuration=$Configuration -p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Build driver gagal (periksa pesan error di atas; pastikan WDK terpasang)." }

$inf = Get-ChildItem "$PSScriptRoot\sysvad\ExterAudio" -Recurse -Filter ExterVirtualAudio.inf |
       Where-Object { $_.FullName -match "\\x64\\$Configuration\\" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $inf) { throw "ExterVirtualAudio.inf tidak ditemukan di output build." }
$pkg = $inf.DirectoryName
foreach ($f in 'ExterVirtualAudio.sys', 'ExterVirtualAudio.cat') {
    if (-not (Test-Path (Join-Path $pkg $f))) { Write-Warning "$f tidak ada di $pkg (paket belum lengkap; katalog dibuat oleh Inf2Cat saat build berhasil)." }
}

$dist = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\driver'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
Copy-Item "$pkg\*" $dist -Recurse
Write-Host "Paket driver: $dist" -ForegroundColor Green
Write-Host "Pasang: powershell -File Exter.Installer\Install-ExterVirtualMic.ps1 -DriverDir `"$dist`"  (tambahkan -TrustSigner untuk sertifikat test)"
