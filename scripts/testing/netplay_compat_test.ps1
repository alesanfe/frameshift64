# Compatibility-handshake test: a LAN host and a joiner whose
# SSB64_COMPAT_TOKEN is forced. Default mode: a different token -> somebody
# must reject with a build/mod-mismatch failure and no match may start.
# -Legacy mode: the joiner sends a pre-handshake PICK (no token field) and
# answers host tokens like an old build -> the match must still start.
#
#   .\scripts\testing\netplay_compat_test.ps1
#   .\scripts\testing\netplay_compat_test.ps1 -Legacy
#
# Exit code: 0 expected outcome, 2 unexpected outcome.
param(
    [switch]$Legacy,
    [int]$Port = 47500,
    [string]$BuildDir = "$PSScriptRoot\..\..\build\x64\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\build\x64\compat-test"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir
Remove-Item "$out\*" -ErrorAction SilentlyContinue

function Start-Peer([string]$name, [int]$fighter, [hashtable]$extra) {
    $vars = @{
        SSB64_LOBBY_FKIND = "$fighter"; SSB64_LOBBY_AUTOSTART = '1'; SSB64_LOBBY_MIN_PLAYERS = '2'
        SSB64_NETPLAY_FUZZ = '111'; SSB64_MUTE = '1'; SSB64_NO_UPDATE_CHECK = '1'
        SSB64_LOG_PATH = "$out\$name.log"; SSB64_SAVE_PATH = "$out\$name.sav"
    }
    foreach ($k in $extra.Keys) { $vars[$k] = $extra[$k] }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $build 'BattleShip.exe'
    $psi.WorkingDirectory = $build
    $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

$host_ = Start-Peer 'host' 0 @{ SSB64_LAN_HOST = "$Port" }
Start-Sleep -Seconds 2
$client = Start-Peer 'client1' 1 @{ SSB64_LAN_JOIN = "127.0.0.1:$Port"; SSB64_COMPAT_TOKEN = $(if ($Legacy) { 'none' } else { 'bogus:0000000000000000' }) }
Start-Sleep -Seconds $(if ($Legacy) { 20 } else { 10 })
foreach ($p in @($host_, $client)) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 2
foreach ($p in @($host_, $client)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }

foreach ($f in @('host', 'client1')) {
    Write-Output "--- $f"
    Select-String -Path "$out\$f.log" -Pattern 'SSB64 Lobby:|mismatch|online match' -ErrorAction SilentlyContinue |
        Select-Object -Last 8 | ForEach-Object { $_.Line }
}

$logs = (Get-Content "$out\host.log" -ErrorAction SilentlyContinue) +
        (Get-Content "$out\client1.log" -ErrorAction SilentlyContinue)
if ($Legacy) {
    if ($logs -match 'online match' -and -not ($logs -match 'mismatch')) {
        Write-Output 'PASS: legacy (tokenless) peer interoperates - match started'
        exit 0
    }
    Write-Output 'FAIL: legacy peer was rejected or the match never started'
    exit 2
}
# Either side may reject first: a client can see the host's PICK before its own
# PICK reaches the host. What matters is somebody rejected and no match ran.
if (($logs -match 'mismatch') -and -not ($logs -match 'online match')) {
    Write-Output 'PASS: incompatible build/mod token rejected before the match'
    exit 0
}
Write-Output 'FAIL: mismatch was not rejected'
exit 2
