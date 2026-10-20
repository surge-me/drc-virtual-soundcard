# probe_audio_env.ps1 — R3/R4 环境探针（只读）
#
# 用途：判定「本会话是否可见音频端点」，用于解释 router_tests R3/R4 的 SKIP。
# 两条独立信号：winmm 输出设备数、MMDevice 端点 PnP 设备数、以及会话状态。
# 只读：不改注册表、不动服务、不装驱动。
$ErrorActionPreference = 'Continue'

Write-Output "=== winmm（独立 API 路径）==="
$src = @"
using System;
using System.Runtime.InteropServices;
public static class Aud {
  [DllImport("winmm.dll")] public static extern int waveOutGetNumDevs();
  [DllImport("winmm.dll")] public static extern int waveInGetNumDevs();
}
"@
Add-Type -TypeDefinition $src
Write-Output ("waveOutGetNumDevs = {0}" -f [Aud]::waveOutGetNumDevs())
Write-Output ("waveInGetNumDevs  = {0}" -f [Aud]::waveInGetNumDevs())

Write-Output "=== AudioEndpoint PnP 设备（机器级）==="
$ep = Get-CimInstance Win32_PnPEntity -Filter "PNPClass='AudioEndpoint'" -ErrorAction SilentlyContinue
Write-Output ("AudioEndpoint count = {0}" -f ($ep | Measure-Object).Count)

Write-Output "=== 会话 ==="
qwinsta 2>&1 | Out-String | Write-Output

Write-Output "=== MMDevices 端点注册表状态（DeviceState: 1=ACTIVE 2=DISABLED 4=NOTPRESENT 8=UNPLUGGED）==="
foreach ($flow in @('Render','Capture')) {
  Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow" -ErrorAction SilentlyContinue | ForEach-Object {
    $st = (Get-ItemProperty -Path $_.PSPath).DeviceState
    $name = (Get-ItemProperty -Path ($_.PSPath + '\Properties') -ErrorAction SilentlyContinue).'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    Write-Output ("  {0} {1} state={2} name={3}" -f $flow, $_.PSChildName, $st, $name)
  }
}
