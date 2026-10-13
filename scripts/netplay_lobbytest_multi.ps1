# Multi-player end-to-end test against the Cloudflare lobby
# (server/cloudflare, protocol v2): a host creates a room and up to three
# clients join by code. The host waits for -Players participants, starts a
# rollback match with fuzzed inputs (star topology: clients link only to
# the host, which forwards input datagrams), then every peer's
# confirmed-state trace is compared pairwise.
#
#   .\scripts\netplay_lobbytest_multi.ps1 -Players 4 -Seconds 60
#   .\scripts\netplay_lobbytest_multi.ps1 -Players 3 -ForceRelay
#
# Without -Server it uses the local Node lobby (server/node-lobby.js) on
# ws://127.0.0.1:8799, starting it if nothing answers there. `wrangler dev`'s
# local ws proxy drops binary relay frames, so it cannot host these tests;
# pass -Server explicitly to point at a real deployment.
#
# Exit code: 0 in sync, 1 desync, 2 no match / missing trace.
param(
    [int]$Players = 3,
    [int]$Seconds = 60,
    [int]$LatencyMs = 0,
    [int]$JitterMs = 0,
    [int]$LossPct = 0,
    [string]$Server = "",
    [int]$Stage = 0,
    [switch]$ForceRelay,
    [switch]$DebugStages,
    [string]$BuildDir = "$PSScriptRoot\..\build\x64\Release",
    [string]$OutDir = "$PSScriptRoot\..\build\x64\lobby-test-multi"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir
Remove-Item "$out\*" -ErrorAction SilentlyContinue

$wrangler = $null
if (-not $Server) {
    $Server = "ws://127.0.0.1:8799"
    $up = $false
    try { $up = (Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8799/health" -TimeoutSec 3).StatusCode -eq 200 } catch {}
    if (-not $up) {
        if (Test-Path "$PSScriptRoot\..\server\cloudflare\node_modules\ws") {
            $wrangler = Start-Process -FilePath "node.exe" -ArgumentList "server\node-lobby.js 8799" `
                -WorkingDirectory "$PSScriptRoot\.." -RedirectStandardOutput "$out\server.log" `
                -RedirectStandardError "$out\server.err" -PassThru -WindowStyle Hidden
        } else {
            $wrangler = Start-Process -FilePath "cmd.exe" -ArgumentList "/c npx wrangler dev --port 8799 --ip 127.0.0.1" `
                -WorkingDirectory "$PSScriptRoot\..\server\cloudflare" -RedirectStandardOutput "$out\server.log" `
                -RedirectStandardError "$out\server.err" -PassThru -WindowStyle Hidden
        }
        for ($i = 0; $i -lt 30 -and -not $up; $i++) {
            Start-Sleep -Seconds 1
            try { $up = (Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:8799/health" -TimeoutSec 2).StatusCode -eq 200 } catch {}
        }
        if (-not $up) { Write-Output "lobby server did not start"; exit 2 }
    }
}
Write-Output "lobby server: $Server   players: $Players"

function Start-Peer([string]$name, [int]$fighter, [int]$fuzz, [bool]$isHost) {
    $vars = @{
        SSB64_LOBBY_CODE_FILE = "$out\code.txt"; SSB64_LOBBY_FKIND = "$fighter"; SSB64_LOBBY_STAGE = "$Stage"
        SSB64_NETPLAY_FUZZ = "$fuzz"; SSB64_MUTE = '1'; SSB64_NO_UPDATE_CHECK = '1'
        SSB64_NETPLAY_SIM_LATENCY_MS = "$LatencyMs"; SSB64_NETPLAY_SIM_JITTER_MS = "$JitterMs"
        SSB64_NETPLAY_SIM_LOSS_PCT = "$LossPct"; SSB64_NETPLAY_SIM_SEED = "$fuzz"
        SSB64_NETPLAY_CONFIRMED_TRACE = "$out\$name.trace"; SSB64_LOG_PATH = "$out\$name.log"
        SSB64_SAVE_PATH = "$out\$name.sav"
    }
    if ($ForceRelay) { $vars['SSB64_LOBBY_FORCE_RELAY'] = '1' }
    if ($DebugStages) { $vars['SSB64_NETPLAY_DEBUG_STAGES'] = '1' }
    if ($isHost) {
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
$procs += Start-Peer 'host' 0 111 $true
Start-Sleep -Seconds 3
for ($i = 1; $i -lt $Players; $i++) {
    $procs += Start-Peer "client$i" $i (200 + $i) $false
    Start-Sleep -Milliseconds 800
}
Start-Sleep -Seconds $Seconds
foreach ($p in $procs) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
if ($wrangler) { & taskkill /PID $wrangler.Id /T /F | Out-Null }

$names = @('host') + @(1..($Players - 1) | ForEach-Object { "client$_" })
foreach ($f in $names) {
    Write-Output "--- $f"
    Select-String -Path "$out\$f.log" -Pattern 'SSB64 Lobby:|SSB64 WS:|online match|netplay tick|DESYNC|rollback FAILED|dropping|silent' -ErrorAction SilentlyContinue |
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
Write-Output "IN SYNC across $Players players for $worst ticks"
exit 0
