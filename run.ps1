# Launch the recompiled build.
#
# game_data_root is mandatory: the documented argv[1] / "assets" fallbacks do
# not fire in SDK v0.10.0, and without the flag the process exits immediately
# with "--game_data_root was not provided." in logs/.
#
# metadata_root points at the achievement data extracted from the XEX. The
# runtime otherwise looks beside the game data, not in the project.
#
# gpu_plugin is mandatory too. The Xenos backend is loaded at runtime rather
# than linked, and the cvar defaults to empty, so without it the runtime comes
# up in "native rendering mode": the window is black and every Vd* kernel call
# logs "no GPU emulation loaded".
#
# Windowed by default. The SDK's `fullscreen` cvar defaults to TRUE, so a bare
# launch takes over the whole display. Pass -Fullscreen to opt back in.
#
# mnk_mode turns on keyboard control of the virtual pad, and defaults to FALSE
# in the SDK - so without it the keyboard does nothing at all and the game can
# only be played with a controller. Start is Enter or X, A is Space, the left
# stick is WASD, the d-pad is Shift+arrows.

param(
    [ValidateSet("local-debug", "local-relwithdebinfo", "local-release")]
    [string]$Config = "local-relwithdebinfo",

    [string]$GameRoot = "D:\Programming\GitHub\NBA JAM On Fire Edition\Root",

    # The SDK default is fullscreen; this script inverts it.
    [switch]$Fullscreen,

    [int]$Width = 1280,
    [int]$Height = 720,

    # Extra flags passed straight through, e.g. --log_level=debug
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Extra
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "out\build\$Config\nbajam_ofe.exe"

if (-not (Test-Path $exe)) {
    throw "Not built: $exe`nRun: cmake --build --preset $Config -j 10"
}
if (-not (Test-Path $GameRoot)) {
    throw "Game data not found: $GameRoot"
}

# Both paths contain spaces, and Start-Process does not quote array elements,
# so each value carries its own quotes or the path is split at the first space.
# Not named $args: that is a PowerShell automatic variable.
$launchArgs = @(
    "--game_data_root=`"$GameRoot`""
    "--metadata_root=`"$(Join-Path $here 'metadata')`""
    "--gpu_plugin=xenos"
    "--mnk_mode=true"
)
if ($Fullscreen) {
    $launchArgs += "--fullscreen=true"
} else {
    $launchArgs += @("--fullscreen=false", "--window_width=$Width", "--window_height=$Height")
}
# $Extra is $null when no extra flags were passed; concatenating it straight in
# puts a null element in the array and Start-Process rejects the whole thing.
if ($Extra) { $launchArgs += @($Extra | Where-Object { $_ }) }

Write-Host "Launching $exe"
Write-Host "  game data: $GameRoot"
Write-Host "  display:   $(if ($Fullscreen) { 'fullscreen' } else { "windowed ${Width}x${Height}" })"
if ($Extra) { Write-Host "  extra:     $($Extra -join ' ')" }

# The process runs from the game root, the folder default.xex sits in, so a
# root is a self-contained thing: data, executable's working directory and
# saves in one place. The runtime's own log and crash report still go beside
# the executable, which is where they are read from.
$logDir = Join-Path (Split-Path -Parent $exe) "logs"
$before = @(Get-ChildItem $logDir -Filter *.log -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Name })

# This is a /SUBSYSTEM:WINDOWS binary, so it detaches immediately. Wait on the
# process object, or we end up tailing the previous run's log.
$proc = Start-Process -FilePath $exe -ArgumentList $launchArgs `
        -WorkingDirectory $GameRoot -PassThru
$proc.WaitForExit()
$code = $proc.ExitCode

$log = Get-ChildItem $logDir -Filter *.log -ErrorAction SilentlyContinue |
       Where-Object { $before -notcontains $_.Name } |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) {
    $log = Get-ChildItem $logDir -Filter *.log -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if ($log) {
    Write-Host "`n--- tail of $($log.Name) ---"
    Get-Content $log.FullName -Tail 25
}

$stack = Join-Path (Split-Path -Parent $exe) "crash_stack.txt"
if (Test-Path $stack) {
    Write-Host "`n--- crash_stack.txt ---"
    Get-Content $stack -Tail 25
}

Write-Host "`nexit code: $code"
