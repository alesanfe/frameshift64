# Runs two BattleShip instances against each other over localhost UDP with
# rollback netplay, seeded random inputs on both sides and (optionally) a
# simulated bad network, then diffs the two peers' confirmed-state traces.
# Identical traces = the peers never desynced. See docs/rollback_design.md.
#
#   .\scripts\testing\netplay_localtest.ps1 -Seconds 60 -LatencyMs 50 -JitterMs 20 -LossPct 5
#   .\scripts\testing\netplay_localtest.ps1 -DupPct 5 -ReorderPct 10 -CorruptPct 1
#
# Exit code: 0 in sync, 1 desync, 2 a peer produced no trace.
param(
    [int]$Seconds = 60,
    [int]$Delay = 2,
    [int]$LatencyMs = 0,
    [int]$JitterMs = 0,
    [int]$LossPct = 0,
    [int]$DupPct = 0,
    [int]$ReorderPct = 0,
    [int]$ReorderWindowMs = 100,
    [int]$CorruptPct = 0,
    [int]$Seed = 4242,
    [int]$BasePort = 7801,
    [int]$InjectDesync = -1,
    # Mutation testing: disables one validation so the run must DESYNC.
    # accept_bad_session | accept_bad_checksum | no_forward_guard
    [string]$Mutant = "",
    # Injects <n> seeded pseudo-random datagrams per drain into the packet
    # parser (tiers from pure garbage to checksum-valid). Oracle = no crash.
    [int]$PacketFuzz = 0,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\netplay-test"
)

$build = Resolve-Path $BuildDir
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = Resolve-Path $OutDir

function Start-Peer([int]$local, [int]$remote, [int]$bindPort, [int]$peerPort, [int]$isHost, [int]$fuzz, [string]$name) {
    $vars = @{
        SSB64_NETPLAY = '1'; SSB64_NETPLAY_BOOTSTRAP = '1'; SSB64_NETPLAY_HOST = "$isHost"; SSB64_MUTE = '1'; SSB64_NO_UPDATE_CHECK = '1'
        SSB64_NETPLAY_LOCAL_PLAYER = "$local"; SSB64_NETPLAY_REMOTE_PLAYER = "$remote"
        SSB64_NETPLAY_BIND = "127.0.0.1:$bindPort"; SSB64_NETPLAY_PEER = "127.0.0.1:$peerPort"
        SSB64_NETPLAY_DELAY = "$Delay"; SSB64_NETPLAY_SEED = "$Seed"; SSB64_NETPLAY_FUZZ = "$fuzz"
        SSB64_NETPLAY_SIM_LATENCY_MS = "$LatencyMs"; SSB64_NETPLAY_SIM_JITTER_MS = "$JitterMs"
        SSB64_NETPLAY_SIM_LOSS_PCT = "$LossPct"; SSB64_NETPLAY_SIM_SEED = "$fuzz"
        SSB64_NETPLAY_SIM_DUP_PCT = "$DupPct"; SSB64_NETPLAY_SIM_REORDER_PCT = "$ReorderPct"
        SSB64_NETPLAY_SIM_REORDER_WINDOW_MS = "$ReorderWindowMs"; SSB64_NETPLAY_SIM_CORRUPT_PCT = "$CorruptPct"
        SSB64_NETPLAY_CONFIRMED_TRACE = "$out\$name.trace"; SSB64_LOG_PATH = "$out\$name.log"
        SSB64_SAVE_PATH = "$out\$name.sav"
        SSB64_NETPLAY_TEST_DESYNC = $(if ($isHost -eq 1) { "$InjectDesync" } else { "-1" })
        SSB64_NETPLAY_MUTANT = $Mutant
        SSB64_NETPLAY_PACKETFUZZ = "$PacketFuzz"
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $build 'BattleShip.exe'
    $psi.WorkingDirectory = $build
    $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

Remove-Item "$out\*.trace", "$out\*.log" -ErrorAction SilentlyContinue
$a = Start-Peer 0 1 $BasePort ($BasePort + 1) 1 111 'host'
$b = Start-Peer 1 0 ($BasePort + 1) $BasePort 0 222 'client'
Start-Sleep -Seconds $Seconds
foreach ($p in @($a, $b)) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($p in @($a, $b)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }

$ta = "$out\host.trace"; $tb = "$out\client.trace"
if (-not (Test-Path $ta) -or -not (Test-Path $tb)) { Write-Output "missing trace"; exit 2 }
$la = Get-Content $ta; $lb = Get-Content $tb
$n = [Math]::Min($la.Count, $lb.Count)
Write-Output "host traced=$($la.Count) client traced=$($lb.Count) compared=$n"
foreach ($f in @('host', 'client')) {
    Select-String -Path "$out\$f.log" -Pattern 'netplay session|netplay tick|session stop|rollback FAILED|DESYNC|TEST injecting|VECTORED|abort|FATAL' | Select-Object -Last 3 | ForEach-Object { "$f> $($_.Line)" }
}
if ($PacketFuzz -gt 0) {
    # Packet-fuzz oracle: malformed and checksum-valid garbage datagrams are
    # injected into the parser; tier-3 packets are indistinguishable from real
    # input traffic, so a desync is EXPECTED. What must not happen is a crash
    # or a missing trace.
    $crashed = @('host', 'client') | Where-Object {
        Select-String -Path "$out\$_.log" -Pattern 'unhandled|second-chance|abort\(\)' -Quiet
    }
    if ($crashed) { Write-Output "CRASH during packet fuzz: $($crashed -join ', ')"; exit 1 }
    if ($n -eq 0) { Write-Output "no ticks traced"; exit 2 }
    Write-Output "PACKETFUZZ survived $PacketFuzz injected datagrams/drain, $n ticks traced (desync expected)"
    exit 0
}
for ($i = 0; $i -lt $n; $i++) {
    # A killed process can leave its last trace line half-written; a partial
    # line is a teardown artifact, not a desync — skip it.
    if (($la[$i] -split '\s+').Count -lt 13 -or ($lb[$i] -split '\s+').Count -lt 13) {
        Write-Output "skipping truncated trace line $i"
        continue
    }
    if ($la[$i] -ne $lb[$i]) {
        Write-Output "DESYNC at line $i"
        Write-Output "  host:   $($la[$i])"
        Write-Output "  client: $($lb[$i])"
        exit 1
    }
}
if ($n -eq 0) { Write-Output "no ticks traced"; exit 2 }
Write-Output "IN SYNC for $n ticks"
exit 0
