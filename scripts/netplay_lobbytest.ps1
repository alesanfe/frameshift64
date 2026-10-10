# End-to-end online test: starts the rendezvous server, then a host that
# creates a room and a client that joins it by code. They hole-punch, pick
# fighters, start a rollback match with fuzzed inputs, and their
# confirmed-state traces are compared. -LatencyMs/-LossPct simulate a bad
# network on the game traffic (the lobby handshake uses the same socket).
#
#   .\scripts\netplay_lobbytest.ps1 -Seconds 60
#
# Exit code: 0 in sync, 1 desync, 2 no match / no trace.
param(
    [int]$Seconds = 60,
    [int]$LatencyMs = 0,
    [int]$JitterMs = 0,
    [int]$LossPct = 0,
    [int]$ServerPort = 7000,
    [int]$HostFighter = 0,
    [int]$ClientFighter = 1,
    [int]$Stage = 0,
    [switch]$ForceRelay,
    [string]$BuildDir = "$PSScriptRoot\..\build\us\Release",
    [string]$OutDir = "$PSScriptRoot\..\build\us\lobby-test"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir
Remove-Item "$out\*" -ErrorAction SilentlyContinue

$server = Start-Process -FilePath node -ArgumentList "`"$PSScriptRoot\..\server\lobby-server.js`" $ServerPort" `
    -RedirectStandardOutput "$out\server.log" -RedirectStandardError "$out\server.err" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1

function Start-Peer([string]$role, [int]$fighter, [int]$fuzz) {
    $vars = @{
        SSB64_LOBBY_CODE_FILE = "$out\code.txt"; SSB64_LOBBY_FKIND = "$fighter"; SSB64_LOBBY_STAGE = "$Stage"
        SSB64_NETPLAY_FUZZ = "$fuzz"
        SSB64_NETPLAY_SIM_LATENCY_MS = "$LatencyMs"; SSB64_NETPLAY_SIM_JITTER_MS = "$JitterMs"
        SSB64_NETPLAY_SIM_LOSS_PCT = "$LossPct"; SSB64_NETPLAY_SIM_SEED = "$fuzz"
        SSB64_NETPLAY_CONFIRMED_TRACE = "$out\$role.trace"; SSB64_LOG_PATH = "$out\$role.log"
        SSB64_SAVE_PATH = "$out\$role.sav"
        SSB64_LOBBY_FORCE_RELAY = $(if ($ForceRelay) { "1" } else { "" })
    }
    if ($role -eq 'host') {
        $vars['SSB64_LOBBY_HOST'] = "127.0.0.1:$ServerPort"; $vars['SSB64_LOBBY_AUTOSTART'] = '1'
    } else {
        $vars['SSB64_LOBBY_JOIN'] = "127.0.0.1:$ServerPort"
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $build 'BattleShip.exe'
    $psi.WorkingDirectory = $build
    $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

$a = Start-Peer 'host' $HostFighter 111
Start-Sleep -Seconds 2
$b = Start-Peer 'client' $ClientFighter 222
Start-Sleep -Seconds $Seconds
foreach ($p in @($a, $b)) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($p in @($a, $b)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue

Write-Output "--- server"
Get-Content "$out\server.log" -ErrorAction SilentlyContinue
foreach ($f in @('host', 'client')) {
    Write-Output "--- $f"
    Select-String -Path "$out\$f.log" -Pattern 'SSB64 Lobby:|online match|netplay tick|DESYNC|rollback FAILED' |
        Select-Object -Last 8 | ForEach-Object { $_.Line }
}
$ta = "$out\host.trace"; $tb = "$out\client.trace"
if (-not (Test-Path $ta) -or -not (Test-Path $tb)) { Write-Output "no trace (match did not start)"; exit 2 }
$la = Get-Content $ta; $lb = Get-Content $tb
$n = [Math]::Min($la.Count, $lb.Count)
Write-Output "host traced=$($la.Count) client traced=$($lb.Count)"
for ($i = 0; $i -lt $n; $i++) {
    if ($la[$i] -ne $lb[$i]) {
        Write-Output "DESYNC at line $i`n  host:   $($la[$i])`n  client: $($lb[$i])"
        exit 1
    }
}
if ($n -eq 0) { Write-Output "no ticks traced"; exit 2 }
Write-Output "IN SYNC for $n ticks"
exit 0
