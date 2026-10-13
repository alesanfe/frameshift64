# LAN end-to-end test: direct IP, no lobby server, no STUN, no WebSocket.
# The host binds a UDP port; clients dial it with SSB64_LAN_JOIN=ip:port and
# the host assigns room slots on first contact (SSBL HELLO -> HELLOACK slot).
#
#   .\scripts\testing\netplay_lan_test.ps1 -Players 3 -Seconds 45
#
# Exit code: 0 in sync, 1 desync, 2 no match / missing trace.
param(
    [int]$Players = 2,
    [int]$Seconds = 45,
    [int]$Port = 47100,
    [int]$LatencyMs = 0,
    [int]$JitterMs = 0,
    [int]$LossPct = 0,
    [switch]$DebugStages,
    [int]$ReplayFrames = 0,
    [int]$DesyncTick = -1,
    [string]$BuildDir = "$PSScriptRoot\..\..\build\x64\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\build\x64\lan-test"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir
Remove-Item "$out\*" -ErrorAction SilentlyContinue

Write-Output "LAN test: host on 127.0.0.1:$Port   players: $Players"

function Start-Peer([string]$name, [int]$fighter, [int]$fuzz, [bool]$isHost) {
    $vars = @{
        SSB64_LOBBY_FKIND = "$fighter"; SSB64_LOBBY_STAGE = '0'
        SSB64_LOBBY_AUTOSTART = '1'; SSB64_LOBBY_MIN_PLAYERS = "$Players"
        SSB64_NETPLAY_FUZZ = "$fuzz"; SSB64_MUTE = '1'; SSB64_NO_UPDATE_CHECK = '1'
        SSB64_NETPLAY_SIM_LATENCY_MS = "$LatencyMs"; SSB64_NETPLAY_SIM_JITTER_MS = "$JitterMs"
        SSB64_NETPLAY_SIM_LOSS_PCT = "$LossPct"; SSB64_NETPLAY_SIM_SEED = "$fuzz"
        SSB64_NETPLAY_CONFIRMED_TRACE = "$out\$name.trace"; SSB64_LOG_PATH = "$out\$name.log"
        SSB64_SAVE_PATH = "$out\$name.sav"
    }
    if ($DebugStages) { $vars['SSB64_NETPLAY_DEBUG_STAGES'] = '1' }
    if ($ReplayFrames -gt 0) { $vars['SSB64_REPLAY_RECORD_FRAMES'] = "$ReplayFrames" }
    if (-not $isHost -and $DesyncTick -ge 0) { $vars['SSB64_NETPLAY_TEST_DESYNC'] = "$DesyncTick" }
    if ($isHost) { $vars['SSB64_LAN_HOST'] = "$Port" } else { $vars['SSB64_LAN_JOIN'] = "127.0.0.1:$Port" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $build 'BattleShip.exe'
    $psi.WorkingDirectory = $build
    $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

$procs = @()
$procs += Start-Peer 'host' 0 111 $true
Start-Sleep -Seconds 2
for ($i = 1; $i -lt $Players; $i++) {
    $procs += Start-Peer "client$i" $i (200 + $i) $false
    Start-Sleep -Milliseconds 800
}
Start-Sleep -Seconds $Seconds
foreach ($p in $procs) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }

$names = @('host') + @(1..($Players - 1) | ForEach-Object { "client$_" })
foreach ($f in $names) {
    Write-Output "--- $f"
    Select-String -Path "$out\$f.log" -Pattern 'SSB64 Lobby:|online match|netplay tick|DESYNC|rollback FAILED|dropping|silent' -ErrorAction SilentlyContinue |
        Select-Object -Last 8 | ForEach-Object { $_.Line }
}

$traces = @{}
foreach ($f in $names) {
    $t = "$out\$f.trace"
    if (-not (Test-Path $t)) { Write-Output "no trace for $f (match did not start)"; exit 2 }
    $traces[$f] = Get-Content $t
}
$ref = $traces['host']
Write-Output "traced ticks: $(($names | ForEach-Object { "$_=$($traces[$_].Count)" }) -join ' ')"
$worst = 0
foreach ($f in $names) {
    if ($f -eq 'host') { continue }
    $lb = $traces[$f]
    $n = [Math]::Min($ref.Count, $lb.Count)
    for ($i = 0; $i -lt $n; $i++) {
        if ($ref[$i] -ne $lb[$i]) {
            Write-Output "DESYNC host vs $f at line $i`n  host: $($ref[$i])`n  ${f}: $($lb[$i])"
            exit 1
        }
    }
    $worst = [Math]::Max($worst, $n)
}
if ($worst -eq 0) { Write-Output "no ticks traced"; exit 2 }
Write-Output "IN SYNC across $Players players for $worst ticks (LAN direct)"
exit 0
