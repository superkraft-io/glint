param(
  [Parameter(Mandatory = $true)]
  [string]$Path
)

# 1) Quick existence + quick "not locked" check
if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Write-Host "not_found"; exit 2 }
try {
  $fs = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
  $fs.Close()
  Write-Host "free"
  exit 0
} catch {
  # might be locked or permission issue; continue to ask Restart Manager who uses it
}

# 2) Ask Restart Manager which processes are using the file
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class RM {
  [DllImport("Rstrtmgr.dll", CharSet=CharSet.Unicode)]
  static extern int RmStartSession(out uint h, int flags, string key);
  [DllImport("Rstrtmgr.dll", CharSet=CharSet.Unicode)]
  static extern int RmRegisterResources(uint h, uint nFiles, string[] files, uint nApps, IntPtr apps, uint nSvcs, IntPtr svcs);
  [StructLayout(LayoutKind.Sequential)] public struct RM_UNIQUE_PROCESS { public int dwProcessId; public System.Runtime.InteropServices.ComTypes.FILETIME ProcessStartTime; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct RM_PROCESS_INFO {
    public RM_UNIQUE_PROCESS Process;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string AppName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=64)] public string ServiceShortName;
    public uint AppType, AppStatus, TSSessionId, bRestartable;
  }
  [DllImport("Rstrtmgr.dll", CharSet=CharSet.Unicode)]
  static extern int RmGetList(uint h, out uint needed, ref uint have, [In, Out] RM_PROCESS_INFO[] infos, ref uint reasons);
  [DllImport("Rstrtmgr.dll")] static extern int RmEndSession(uint h);

  public static RM_PROCESS_INFO[] Who(string path) {
    uint h; RmStartSession(out h, 0, Guid.NewGuid().ToString());
    try {
      RmRegisterResources(h, 1, new[]{path}, 0, IntPtr.Zero, 0, IntPtr.Zero);
      uint need=0, have=0, reasons=0;
      RmGetList(h, out need, ref have, null, ref reasons);
      var arr = new RM_PROCESS_INFO[need];
      have = need;
      RmGetList(h, out need, ref have, arr, ref reasons);
      return arr;
    } finally { RmEndSession(h); }
  }
}
"@ | Out-Null

$procs = [RM]::Who($Path)

if (-not $procs -or $procs.Length -eq 0) {
  Write-Host "locked,-1:unknown"; exit 1
}

Write-Host ("locked," + (($procs | ForEach-Object { "$($_.Process.dwProcessId):$($_.AppName)" }) -join ''))

exit 1