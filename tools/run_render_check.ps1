# Process-local controller script and native render-target capture. No host input.
param(
    [ValidateSet('default','auto','d3d12','vulkan')] [string]$Renderer = 'default',
    [ValidateRange(1,3)] [int]$Scale = 3,
    [ValidateRange(640,7680)] [int]$Width = 1280,
    [ValidateRange(480,4320)] [int]$Height = 720,
    [string]$Name = 'render-check',
    [ValidateRange(10,180)] [int]$Seconds = 80,
    [ValidateRange(0,179000)] [int]$CaptureAfterMs = 60000,
    [ValidateRange(50,2000)] [int]$InputHoldMs = 150,
    [string]$InputScript = '8000:START;3000:START;3000:START;3000:START;3000:START;3000:START;2500:A;2500:A;2500:A;2500:A;2500:START;2500:A;2500:A;2500:START;3000:A;3000:A;3000:A;3000:A;3000:A;3000:A'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
if ($Name -notmatch '^[a-zA-Z0-9_-]+$') { throw 'Name must be a simple directory name' }
if ($CaptureAfterMs -ge $Seconds * 1000) { throw 'Capture must precede the watchdog' }
$run = Join-Path $repo ('.local\runs\' + $Name)
New-Item -ItemType Directory -Path $run,(Join-Path $repo '.local\tmp') -Force | Out-Null
$env:TEMP = Join-Path $repo '.local\tmp'
$env:TMP = $env:TEMP
$env:LOCALAPPDATA = Join-Path $repo '.local\profile\Local'
$env:APPDATA = Join-Path $repo '.local\profile\Roaming'
$env:NBAJAM_INPUT_SCRIPT = $InputScript
$env:NBAJAM_INPUT_HOLD = [string]$InputHoldMs
$env:NBAJAM_CAPTURE_PATH = Join-Path $run 'guest.ppm'
$env:NBAJAM_CAPTURE_AFTER_MS = [string]$CaptureAfterMs
$exe = Join-Path $repo 'out\build\local-relwithdebinfo\nbajam_ofe.exe'
$flags = @(
    ('--game_data_root="' + (Join-Path $repo 'Root') + '"')
    ('--user_data_root="' + (Join-Path $run 'user') + '"')
    ('--cache_root="' + (Join-Path $run 'cache') + '"')
    ('--metadata_root="' + (Join-Path $repo 'metadata') + '"')
    ('--log_file="' + (Join-Path $run 'game.log') + '"')
    ('--resolution_scale=' + $Scale)
    ('--draw_resolution_scale_x=' + $Scale)
    ('--draw_resolution_scale_y=' + $Scale)
    '--fullscreen=false'
    ('--window_width=' + $Width)
    ('--window_height=' + $Height)
    '--d3d12_debug=true'
    '--d3d12_break_on_error=false'
    '--headless=true'
)
if ($Renderer -ne 'default') { $flags += '--renderer=' + $Renderer }
$started = Get-Date
$proc = Start-Process -FilePath $exe -ArgumentList $flags -WorkingDirectory $repo -WindowStyle Hidden -PassThru
try {
    $expired = -not $proc.WaitForExit($Seconds * 1000)
} finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}
$proc.Refresh()
$captured = (Test-Path -LiteralPath $env:NBAJAM_CAPTURE_PATH) -and
    (Get-Item -LiteralPath $env:NBAJAM_CAPTURE_PATH).LastWriteTime -ge $started
[pscustomobject]@{Run=$run; ExitCode=$proc.ExitCode; Watchdog=$expired; Capture=$captured}
Get-Content -LiteralPath (Join-Path $run 'game.log') -Tail 12
if (-not $captured) { throw 'No fresh render capture; inspect game.log for the failed run' }
