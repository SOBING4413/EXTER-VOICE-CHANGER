#!/usr/bin/env python3
"""Memeriksa konsistensi konstanta/antarmuka antara C++ engine, header driver, dan C#.
Dijalankan tanpa Windows:  python3 Exter.Tests/check-interface.py"""
import re, sys, pathlib
root = pathlib.Path(__file__).resolve().parent.parent
rd = lambda p: (root / p).read_text(encoding="utf-8")
hdr_if, hdr_tr = rd("Exter.Shared/ExterDriverInterface.h"), rd("Exter.Shared/ExterTransport.h")
eng_h, eng_c = rd("Exter.AudioEngine/AudioEngine.h"), rd("Exter.AudioEngine/AudioEngine.cpp")
link_c, cs_vam, cs_nat = rd("Exter.AudioEngine/ExterDriverLink.cpp"), rd("Exter.VirtualAudio/VirtualAudioManager.cs"), rd("Exter.Core/NativeMethods.cs")
fails = []
def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  -> " + detail))
    if not ok: fails.append(name)
def g(rx, text, grp=1):
    m = re.search(rx, text, re.S); return m.group(grp) if m else None

# 1. ID output khusus engine == C#
a = g(r'#define EX_VIRTUAL_MIC_ID L"([^"]+)"', eng_h); b = g(r'VirtualOutputId = "([^"]+)"', cs_vam)
check("EX_VIRTUAL_MIC_ID == VirtualOutputId", a and a == b, f"{a} vs {b}")
# 2. nama endpoint
a = g(r'EXTER_ENDPOINT_FRIENDLY_NAME L"([^"]+)"', hdr_if); b = g(r'VirtualMicName = "([^"]+)"', cs_vam)
check("nama endpoint header == C#", a and a == b, f"{a} vs {b}")
check("guard loopback engine memakai nama endpoint yang sama", f'L"{a}"' in eng_c)
# 3. HRESULT loopback
a = g(r'kErrLoopback = \(HRESULT\)(0x[0-9A-Fa-f]+)', eng_c); b = g(r'ErrLoopback\s*=\s*unchecked\(\(int\)(0x[0-9A-Fa-f]+)\)', cs_vam)
check("kErrLoopback == ErrLoopback", a and b and int(a,16) == int(b,16), f"{a} vs {b}")
# 4. HRESULT dari Win32
def hr_from_win32(n): return 0x80070000 | n
check("ErrDriverNotInstalled == HRESULT(ERROR_FILE_NOT_FOUND=2)",
      int(g(r'ErrDriverNotInstalled\s*=\s*unchecked\(\(int\)(0x[0-9A-Fa-f]+)\)', cs_vam),16) == hr_from_win32(2))
check("ErrAccessDenied == HRESULT(ERROR_ACCESS_DENIED=5)",
      int(g(r'ErrAccessDenied\s*=\s*unchecked\(\(int\)(0x[0-9A-Fa-f]+)\)', cs_vam),16) == hr_from_win32(5))
check("ErrAbiMismatch == HRESULT(ERROR_REVISION_MISMATCH=1306)",
      int(g(r'ErrAbiMismatch\s*=\s*unchecked\(\(int\)(0x[0-9A-Fa-f]+)\)', cs_vam),16) == hr_from_win32(1306))
# 5. IOCTL: kode fungsi unik, akses sesuai CreateFile
codes = re.findall(r'#define (IOCTL_EXTER_\w+)\s+CTL_CODE\(EXTER_DEVICE_TYPE,\s*(0x[0-9A-Fa-f]+),\s*(METHOD_\w+),\s*([A-Z_ |]+)\)', hdr_if)
check("3 IOCTL terdefinisi", len(codes) == 3, str(codes))
check("kode fungsi IOCTL unik & >= 0x800", len({c[1] for c in codes}) == len(codes) and all(int(c[1],16) >= 0x800 for c in codes))
check("device type di rentang vendor (>=0x8000)", int(g(r'EXTER_DEVICE_TYPE\s+(0x[0-9A-Fa-f]+)u?', hdr_if),16) >= 0x8000)
check("CreateFile minta GENERIC_READ|GENERIC_WRITE (cocok FILE_READ|WRITE_ACCESS)", "GENERIC_READ | GENERIC_WRITE" in link_c)
check("link memakai IOCTL ATTACH dan DETACH", "IOCTL_EXTER_ATTACH_RING" in link_c and "IOCTL_EXTER_DETACH_RING" in link_c)
# 6. path device
nt = g(r'EXTER_DEVICE_SYMLINK_NT\s+L"([^"]+)"', hdr_if); w32 = g(r'EXTER_DEVICE_WIN32_PATH\s+L"([^"]+)"', hdr_if)
check("symlink NT cocok path Win32", nt and w32 and nt.replace("\\\\DosDevices\\\\", "").replace("\\DosDevices\\", "") == w32.replace("\\\\\\\\.\\\\", "").replace("\\\\.\\", ""), f"{nt} / {w32}")
# 7. ABI & format
check("ABI version didefinisikan 1x", hdr_tr.count("#define EXTER_ABI_VERSION") == 1)
check("ukuran struct IOCTL: 16 byte (u32,u32,u64) di C++", "ex_u64 sectionHandle" in hdr_if and "ex_u32 ringBytes" in hdr_if)
# 8. ExStatus & ExParams C++ == C# (urutan & tipe). Salah urutan = nilai tertukar diam-diam lewat P/Invoke.
def fields_c(text, name):
    body = g(r'struct ' + name + r' \{(.*?)\};', text, 1)
    out = []
    for line in body.splitlines():
        line = line.split("//")[0].strip()
        m = re.match(r'(int|float)\s+(\w+);', line)
        if m: out.append((m.group(1), m.group(2).lower()))
    return out
def fields_cs(text, name):
    body = g(r'struct ' + name + r'\s*\{(.*?)\}', text, 1)
    return [({"int":"int","float":"float"}[m.group(1)], m.group(2).lower()) for m in re.finditer(r'public (int|float) (\w+);', body)]
for st in ("ExStatus", "ExParams"):
    fc, fs = fields_c(eng_h, st), fields_cs(cs_nat, st)
    check(f"{st}: urutan & tipe field C++ == C#", fc and fc == fs, f"\n C++: {fc}\n C#:  {fs}")
# 8b. setiap fungsi P/Invoke ada di header DAN diimplementasikan
eng_all = eng_c + rd("Exter.AudioEngine/DeviceEnum.cpp")
for fn in re.findall(r'public static extern \w+ (ex_\w+)\(', cs_nat):
    check(f"P/Invoke {fn} dideklarasikan & diimplementasikan di engine",
          re.search(r'EXAPI[^;{]*\b' + fn + r'\(', eng_h) is not None and re.search(r'EXAPI[^;{]*\b' + fn + r'\(', eng_all) is not None)
# 8c. HRESULT start dari C# punya pesan
check("ErrEntryPoint = HRESULT(ERROR_PROC_NOT_FOUND=127)",
      int(g(r'ErrEntryPoint\s*=\s*unchecked\(\(int\)(0x[0-9A-Fa-f]+)\)', cs_vam),16) == hr_from_win32(127))
# 8d. batas parameter di engine == Settings.Clamp (engine memotong ke batas yang sama dengan UI)
cs_set = rd("Exter.Core/Settings.cs")
for fld, cs_fld, lo, hi in (("highPassHz","HighPassHz",80,1200), ("lowPassHz","LowPassHz",1500,8000),
                            ("gateThresholdDb","GateThresholdDb",-80,-10), ("compThresholdDb","CompThresholdDb",-50,-5),
                            ("compRatio","CompRatio",1,20), ("drive","Drive",1,8), ("staticLevel","StaticLevel",0,1)):
    m_c = re.search(r'c\.' + fld + r'\s*=\s*ClampParam\(c\.' + fld + r',\s*(-?[\d.]+)f,\s*(-?[\d.]+)f', eng_c)
    m_s = re.search(cs_fld + r'\s*=\s*Math\.Clamp\(' + cs_fld + r',\s*(-?[\d.]+)f,\s*(-?[\d.]+)f', cs_set)
    ok = bool(m_c and m_s and float(m_c.group(1)) == float(m_s.group(1)) == lo and float(m_c.group(2)) == float(m_s.group(2)) == hi)
    check(f"batas {fld}: engine == Settings.Clamp == [{lo},{hi}]", ok, f"{m_c and m_c.groups()} vs {m_s and m_s.groups()}")

# 10. DRIVER (Exter.Driver): INF, tabel format, kode kernel, project, installer
drv = "Exter.Driver/sysvad/"
inx   = rd(drv + "ExterAudio/ExterVirtualAudio.inx")
mp_h  = rd(drv + "ExterAudio/minipairs.h")
mtop  = rd(drv + "ExterAudio/micintoptable.h")
mwav  = rd(drv + "ExterAudio/micinwavtable.h")
vmic  = rd(drv + "EndpointsCommon/ExterVmic.cpp")
vmic_h = rd(drv + "EndpointsCommon/ExterVmic.h")
wrs   = rd(drv + "EndpointsCommon/minwavertstream.cpp")
adp   = rd(drv + "adapter.cpp")
vcx_a = rd(drv + "ExterAudio/ExterAudio.vcxproj")
vcx_e = rd(drv + "EndpointsCommon/EndpointsCommon.vcxproj")
ps_i  = rd("Exter.Installer/Install-ExterVirtualMic.ps1")
sln_d = rd("Exter.Driver/Exter.Driver.sln")

friendly = g(r'EXTER_ENDPOINT_FRIENDLY_NAME L"([^"]+)"', hdr_if)
check("INF: MicInCustomName == nama endpoint (dicari engine & aplikasi)", g(r'(?m)^MicInCustomName\s*=\s*"([^"]+)"', inx) == friendly)
check("INF: nama interface == minipairs.h",
      g(r'KSNAME_WaveMicIn\s*=\s*"(\w+)"', inx) in mp_h and g(r'KSNAME_TopologyMicIn\s*=\s*"(\w+)"', inx) in mp_h and 'L"WaveMicIn"' in mp_h and 'L"TopologyMicIn"' in mp_h)
guid_inf = g(r'MicInCustomNameGUID\s*=\s*\{([0-9a-fA-F-]+)\}', inx)
gm = re.search(r'DEFINE_GUID\(MICIN_CUSTOM_NAME,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),((?:\s*0x[0-9a-fA-F]+,?){8})\)', mtop.replace("\r", ""))
guid_h = None
if gm:
    t = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', ",".join(gm.groups()))]
    guid_h = "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x" % tuple(t)
check("INF: GUID nama custom == MICIN_CUSTOM_NAME di micintoptable.h", guid_h and guid_inf and guid_h.lower() == guid_inf.lower(), f"{guid_h} vs {guid_inf}")

hw_inf = g(r'=\s*EXTER_VMIC,\s*(\S+)', inx); hw_ps = g(r"\$HardwareId\s*=\s*'([^']+)'", ps_i)
check("hardware ID INF == installer", hw_inf and hw_inf == hw_ps, f"{hw_inf} vs {hw_ps}")
target = g(r'<TargetName>(\w+)</TargetName>', vcx_a)
check("nama .sys: TargetName == INF (service, binary, copy list, driver key)",
      target == "ExterVirtualAudio" and all(x in inx for x in ("ServiceBinary = %13%\\ExterVirtualAudio.sys", "AddService = ExterVirtualAudio,", "HKR,,Driver,,ExterVirtualAudio.sys")) and "ExterVirtualAudio.sys = 222" in inx)
check("installer mencari .inf/.sys/.cat dengan nama yang sama", all(x in ps_i for x in ("ExterVirtualAudio.inf", "ExterVirtualAudio.sys", "ExterVirtualAudio.cat")))
check("INF: CatalogFile == ExterVirtualAudio.cat", "CatalogFile = ExterVirtualAudio.cat" in inx)

# tabel format mic: HANYA 48 kHz / mono / 16-bit == konstanta ring
fmts = re.findall(r'WAVE_FORMAT_EXTENSIBLE,\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),', mwav)
sr = int(g(r'EXTER_SAMPLE_RATE\s+(\d+)u', hdr_tr)); ch = int(g(r'EXTER_CHANNELS\s+(\d+)u', hdr_tr))
check("mic: tepat 1 format (hanya format ring)", len(fmts) == 1, str(fmts))
check("mic: format == EXTER_SAMPLE_RATE / EXTER_CHANNELS / 16-bit",
      len(fmts) == 1 and (int(fmts[0][0]), int(fmts[0][1]), int(fmts[0][4])) == (ch, sr, 16) and int(fmts[0][2]) == sr * ch * 2 and int(fmts[0][3]) == ch * 2, str(fmts))
check("mic: semua mode memakai format indeks 0 (tanpa 16/24 kHz)", "[2].DataFormat" not in mwav and "[4].DataFormat" not in mwav and "MICIN_MIN_SAMPLE_RATE               48000" in mwav)
check("mic: hanya MicIn didaftarkan (tanpa render/mic array/BT/USB di minipairs.h)",
      "&MicInMiniports" in mp_h and not re.search(r"&(Speaker|Hdmi|Spdif|MicArray|BthHfp|UsbHs|A2dp)\w*Miniports", mp_h) and "g_RenderEndpoints" not in mp_h)

# kode kernel
for name in ("IOCTL_EXTER_ATTACH_RING", "IOCTL_EXTER_DETACH_RING", "IOCTL_EXTER_QUERY_STATUS"):
    check(f"kernel menangani {name}", f"case {name}:" in vmic)
check("kernel: abi mismatch -> STATUS_REVISION_MISMATCH", "EXTER_ABI_VERSION" in vmic and "STATUS_REVISION_MISMATCH" in vmic)
check("kernel: SDDL hanya GRGW untuk pengguna terautentikasi, GA hanya SYSTEM/Admin",
      '(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)' in vmic and "FILE_DEVICE_SECURE_OPEN" in vmic)
check("kernel: halaman dikunci + rundown + owner file object",
      all(x in vmic for x in ("MmProbeAndLockPages", "ExWaitForRundownProtectionRelease", "ExAcquireRundownProtection", "OwnerFile", "IRP_MJ_CLEANUP", "IoIs32bitProcess", "ObReferenceObjectByHandle")))
check("kernel: bukan pembaca di jalur audio yang memakai lock/mutex", not re.search(r"ExAcquireFastMutex|KeAcquire|ExAcquireResource", vmic.split("#pragma code_seg(\"PAGE\")")[0]))
check("stream: capture memakai ExterVmic_FillCapture, tanpa ToneGenerator di WriteBytes",
      "ExterVmic_FillCapture(this" in wrs and "GenerateSine" not in wrs)
check("stream: Run/Stop/Pause/destruktor memanggil hook",
      wrs.count("ExterVmic_StreamStop(this)") == 3 and wrs.count("ExterVmic_StreamRun(this)") == 1)
check("adapter: init/shutdown/detach terpasang; fitur BT/USB/A2DP default mati",
      all(x in adp for x in ("ExterVmic_Initialize(DriverObject)", "ExterVmic_Shutdown();", "ExterVmic_Detach();"))
      and all(re.search(rf"g_Disable{n}\s*=\s*1;", adp) for n in ("BthScoBypass", "UsbSideband", "A2dpSideband")))
check("adapter: tidak ada lagi instalasi endpoint render", "g_RenderEndpoints" not in adp)

# project
check("EndpointsCommon.vcxproj memuat ExterVmic.cpp + include Exter.Shared", "ExterVmic.cpp" in vcx_e and "Exter.Shared" in vcx_e)
check("ExterAudio.vcxproj: wdmsec.lib, include Exter.Shared, rc/inx diganti nama, tanpa hdmi/spdif/TabletAudioSample",
      "wdmsec.lib" in vcx_a and "Exter.Shared" in vcx_a and "ExterAudio.rc" in vcx_a and "hdmitopo" not in vcx_a and "spdiftopo" not in vcx_a and "TabletAudioSample" not in vcx_a)
check("ExterAudio.vcxproj: build mandiri, target Desktop (bukan Universal/Package)",
      "<DriverTargetPlatform>Desktop</DriverTargetPlatform>" in vcx_a and "<DriverTargetPlatform>Universal" not in vcx_a
      and "<RequiresPackageProject>" not in vcx_a and "<SupportsPackaging>false" not in vcx_a)
check("kedua project driver mematikan C4392 (wdm.h __rmpadjust vs compiler baru)", "4392" in vcx_a and "4392" in vcx_e)
check("sln driver merujuk kedua project yang ada",
      all((root / "Exter.Driver" / pth.replace("\\", "/")).exists() for pth in re.findall(r'"(sysvad\\[^"]+\.vcxproj)"', sln_d)) and sln_d.count(".vcxproj") == 2)
check("engine membulatkan ukuran section ke halaman (view kernel kelipatan halaman)", "sectionBytes" in link_c and "4095" in link_c)
inx_files = list((root / "Exter.Driver/sysvad/ExterAudio").glob("*.inx"))
check("tepat satu .inx di project driver", len(inx_files) == 1, str(inx_files))

# 9. tidak ada sisa pihak ketiga
def code_lines(p):
    for l in p.read_text(encoding="utf-8", errors="ignore").splitlines():
        t = l.lstrip()
        if t.startswith(("//", "*", "/*", "#")) and not t.startswith(("#include", "#define", "#pragma", "#if", "#endif", "#else")): continue
        yield l
bad = [str(p.relative_to(root)) for p in root.rglob("*") if p.suffix in {".cs",".cpp",".h",".xaml",".ps1",".vcxproj",".csproj"}
       and "Exter.Tests" not in p.parts and any(re.search(r"CABLE (Input|Output)|VBCABLE|VoiceMeeter|vb-audio\.com", l, re.I) for l in code_lines(p))]
check("tanpa referensi fungsional VB-CABLE/VoiceMeeter di source", not bad, str(bad))
sys.exit(1 if fails else 0)
