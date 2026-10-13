# Train-from-replay smoke test: plays back a recorded .ssr and, with
# SSB64_REPLAY_TAKEOVER=<player>:<tick>, checks the slot flips to live input
# at the requested tick. Without -Takeover it verifies clean playback.
#
#   .\scripts\testing\replay_takeover_test.ps1 -Replay <path> [-Takeover "1:100"]
param(
    [Parameter(Mandatory = $true)][string]$Replay,
    [string]$Takeover = "",
    [int]$Seconds = 20,
    [string]$BuildDir = "$PSScriptRoot\..\..\build\x64\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\build\x64\takeover-test"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir
$log = "$out\playback.log"
Remove-Item $log -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $build 'BattleShip.exe'
$psi.WorkingDirectory = $build
$psi.UseShellExecute = $false
$psi.EnvironmentVariables['SSB64_REPLAY_PLAY'] = (Resolve-Path $Replay).Path
$psi.EnvironmentVariables['SSB64_MUTE'] = '1'
$psi.EnvironmentVariables['SSB64_NO_UPDATE_CHECK'] = '1'
$psi.EnvironmentVariables['SSB64_LOG_PATH'] = $log
if ($Takeover) { $psi.EnvironmentVariables['SSB64_REPLAY_TAKEOVER'] = $Takeover }

$p = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds $Seconds
if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null }
Start-Sleep -Seconds 2
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

Select-String -Path $log -Pattern 'Replay|takeover|playback|FAIL' -ErrorAction SilentlyContinue |
    Select-Object -Last 12 | ForEach-Object { $_.Line }

$lines = Get-Content $log -ErrorAction SilentlyContinue
if ($Takeover) {
    if ($lines -match 'takeover player=') { Write-Output 'PASS: takeover fired'; exit 0 }
    Write-Output 'FAIL: takeover never fired'; exit 2
}
if ($lines -match 'playback verify.*result=PASS') { Write-Output 'PASS: replay verified'; exit 0 }
Write-Output 'FAIL: no verification verdict'; exit 2
