# Spectator e2e: host + N-1 players + 1 spectator over the local node lobby
# (or -Server). The spectator joins with role=spec, must receive the STARTM,
# run the match a few ticks behind, never desync the players, and produce a
# confirmed-state trace identical to the players'.
#
#   .\scripts\testing\spectate_test.ps1 -Players 2 -Seconds 30
param(
    [int]$Players = 2,
    [int]$Seconds = 30,
    [int]$LatencyMs = 0,
    [int]$JitterMs = 0,
    [string]$Server = "",
    [switch]$DebugStages,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\spectate-test"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir
Remove-Item "$out\*" -ErrorAction SilentlyContinue

$serverProc = $null
if (-not $Server) {
    $Server = "ws://127.0.0.1:8799"
    $up = $false
    try { $up = (Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8799/health" -TimeoutSec 3).StatusCode -eq 200 } catch {}
    if (-not $up) {
        $serverProc = Start-Process -FilePath "node.exe" -ArgumentList "services\netplay-lobby\src\node-lobby.js 8799" `
            -WorkingDirectory "$PSScriptRoot\..\.." -RedirectStandardOutput "$out\server.log" `
            -RedirectStandardError "$out\server.err" -PassThru -WindowStyle Hidden
        for ($i = 0; $i -lt 30 -and -not $up; $i++) {
            Start-Sleep -Seconds 1
            try { $up = (Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8799/health" -TimeoutSec 2).StatusCode -eq 200 } catch {}
        }
        if (-not $up) { Write-Output "lobby server did not start"; exit 2 }
    }
}
Write-Output "lobby server: $Server   players: $Players + 1 spec"

function Start-Peer([string]$name, [int]$fighter, [int]$fuzz, [bool]$isHost, [bool]$isSpec) {
    $vars = @{
        SSB64_LOBBY_CODE_FILE = "$out\code.txt"; SSB64_LOBBY_FKIND = "$fighter"
        SSB64_NETPLAY_FUZZ = "$fuzz"; SSB64_MUTE = '1'; SSB64_NO_UPDATE_CHECK = '1'
        SSB64_NETPLAY_SIM_LATENCY_MS = "$LatencyMs"; SSB64_NETPLAY_SIM_JITTER_MS = "$JitterMs"
        SSB64_NETPLAY_CONFIRMED_TRACE = "$out\$name.trace"; SSB64_LOG_PATH = "$out\$name.log"
        SSB64_SAVE_PATH = "$out\$name.sav"
    }
    if ($DebugStages) { $vars['SSB64_NETPLAY_DEBUG_STAGES'] = '1' }
    if ($isSpec) {
        $vars['SSB64_LOBBY_JOIN'] = $Server
        $vars['SSB64_LOBBY_SPECTATE'] = '1'
    } elseif ($isHost) {
        $vars['SSB64_LOBBY_HOST'] = $Server; $vars['SSB64_LOBBY_AUTOSTART'] = '1'
        $vars['SSB64_LOBBY_MIN_PLAYERS'] = "$Players"
    } else {
        $vars['SSB64_LOBBY_JOIN'] = $Server
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $build 'BattleShip.exe'
    $psi.WorkingDirectory = $build
    $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

$procs = @()
$procs += Start-Peer 'host' 0 111 $true $false
Start-Sleep -Seconds 3
$procs += Start-Peer 'spec' 0 999 $false $true     # before players: it must see the STARTM
Start-Sleep -Milliseconds 800
for ($i = 1; $i -lt $Players; $i++) {
    $procs += Start-Peer "client$i" $i (200 + $i) $false $false
    Start-Sleep -Milliseconds 800
}
Start-Sleep -Seconds $Seconds
foreach ($p in $procs) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
if ($serverProc) { & taskkill /PID $serverProc.Id /T /F | Out-Null }

$names = @('host', 'spec') + @(1..($Players - 1) | ForEach-Object { "client$_" })
foreach ($f in $names) {
    Write-Output "--- $f"
    Select-String -Path "$out\$f.log" -Pattern 'SSB64 Lobby:|spectat|Spectat|online match|netplay tick|DESYNC|dropped' -ErrorAction SilentlyContinue |
        Select-Object -Last 10 | ForEach-Object { $_.Line }
}

$traces = @{}
$missing = $false
foreach ($f in $names) {
    $t = "$out\$f.trace"
    if (-not (Test-Path $t)) { Write-Output "no trace for $f (spectate failed?)"; $missing = $true; continue }
    $traces[$f] = Get-Content $t
}
if ($missing) { exit 2 }

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
Write-Output "IN SYNC across $Players players + spectator for $worst ticks"
exit 0
