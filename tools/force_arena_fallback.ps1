# Launch the game with its usual memory address already taken.
#
# The runtime puts the guest's 4 GB of memory at 0x100000000 when it can, and
# somewhere else when something is already there - Proton on a Steam Deck,
# an overlay or an antivirus hook injected into the process, or a mod swap's
# predecessor still holding it. Every "the game has fallen over" report so far
# shows the memory at 0x200000000, so this reproduces that on any machine.
#
# It starts the game suspended, reserves one 64 KB block at 0x100000000 inside
# it, and lets it run. Nothing else about the launch changes.
#
#   .\tools\force_arena_fallback.ps1 -Exe <nbajam_ofe.exe> -GameRoot <root>
#   .\tools\force_arena_fallback.ps1 ... -NoBlock      # same launch, address left free

param(
    [Parameter(Mandatory = $true)] [string]$Exe,
    [Parameter(Mandatory = $true)] [string]$GameRoot,
    [string]$MetadataRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "metadata"),
    [switch]$NoBlock,
    [int]$Seconds = 20
)

$ErrorActionPreference = "Stop"

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class SuspendedLaunch {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
                   dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION {
        public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string dir,
        ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr addr, UIntPtr size,
        uint type, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint ResumeThread(IntPtr t);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint CREATE_SUSPENDED = 0x4;
    const uint MEM_RESERVE = 0x2000;
    const uint PAGE_NOACCESS = 0x1;

    // Returns the process id; `blocked` says whether the reservation landed.
    public static int Start(string exe, string cmd, string dir, bool block,
                            out bool blocked) {
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false,
                            CREATE_SUSPENDED, IntPtr.Zero, dir, ref si, out pi)) {
            throw new System.ComponentModel.Win32Exception();
        }
        blocked = false;
        if (block) {
            IntPtr got = VirtualAllocEx(pi.hProcess, new IntPtr(0x100000000L),
                new UIntPtr(0x10000), MEM_RESERVE, PAGE_NOACCESS);
            blocked = got == new IntPtr(0x100000000L);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
}
'@

$exePath = (Resolve-Path $Exe).Path
$rootPath = (Resolve-Path $GameRoot).Path
$cmd = "`"$exePath`" --game_data_root=`"$rootPath`" --metadata_root=`"$MetadataRoot`" " +
       "--gpu_plugin=xenos --mnk_mode=true --fullscreen=false " +
       "--window_width=1280 --window_height=720"

$logDir = Join-Path (Split-Path -Parent $exePath) "logs"
$before = @(Get-ChildItem $logDir -Filter *.log -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })

$blocked = $false
$id = [SuspendedLaunch]::Start($exePath, $cmd, $rootPath, -not $NoBlock, [ref]$blocked)
Write-Host "pid $id; usual address $(if ($NoBlock) { 'left free' } elseif ($blocked) { 'blocked' } else { 'NOT blocked (reservation failed)' })"

Start-Sleep -Seconds $Seconds
$proc = Get-Process -Id $id -ErrorAction SilentlyContinue
Write-Host "after ${Seconds}s: $(if ($proc) { "running, window '$($proc.MainWindowTitle)'" } else { 'exited' })"

$log = Get-ChildItem $logDir -Filter *.log -ErrorAction SilentlyContinue |
       Where-Object { $before -notcontains $_.FullName } |
       Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) {
    Write-Host "log: $($log.FullName)"
    Get-Content $log.FullName |
        Select-String -Pattern "arena|null page|fallen|access violation|exception" |
        ForEach-Object { "  $_" }
}
# Left running on purpose so the window can be looked at; the caller closes it.
$id
