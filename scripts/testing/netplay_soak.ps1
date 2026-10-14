# Soak / rematch driver (testplan s.14). Two halves:
#
#   -RematchRounds N   - N back-to-back localtest sessions. Each round spawns
#                        fresh processes, reconnects, plays, and tears down -
#                        the rematch cycle exercised through
#                        port_netlobby_after_match is approximated at process
#                        granularity (a full in-game rematch needs UI input,
#                        which the headless rig can't drive yet).
#   -SynctestMinutes M - one long SyncTest run (0 = skip) for snapshot/hash
#                        integrity over time.
#
# Failure = any round desyncs, errors out, hangs past its timeout, crashes,
# or the per-round log grows unboundedly (>200 MB is a leak signature).
#
#   .\scripts\testing\netplay_soak.ps1 -RematchRounds 3 -Seconds 45 -SynctestMinutes 5
#
# Exit: 0 all rounds clean, 1 a round failed, 2 harness error.
param(
    [int]$RematchRounds = 3,
    [int]$Seconds = 45,
    [int]$SynctestMinutes = 0,
    [int]$SynctestDistance = 3,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\netplay-test",
    # Sim conditions applied to every round; vary per round for coverage.
    [int]$LatencyMs = 50,
    [int]$JitterMs = 15,
    [int]$LossPct = 1,
    [int]$DupPct = 1,
    [int]$ReorderPct = 2
)

$root = Resolve-Path "$PSScriptRoot\..\.."
$localtest = Join-Path $root 'scripts\testing\netplay_localtest.ps1'
$synctest = Join-Path $root 'scripts\development\rollback_synctest.ps1'
$failures = @()
$MAX_LOG_BYTES = 200MB

for ($round = 1; $round -le $RematchRounds; $round++) {
    Write-Output "=== rematch round $round/$RematchRounds ==="
    & $localtest -BuildDir $BuildDir -OutDir $OutDir -Seconds $Seconds `
        -Seed (4242 + $round * 101) -BasePort (7801 + $round * 10) `
        -LatencyMs $LatencyMs -JitterMs $JitterMs -LossPct $LossPct `
        -DupPct $DupPct -ReorderPct $ReorderPct | Select-Object -Last 2
    $code = $LASTEXITCODE
    if ($code -ne 0) { $failures += "round $round exit=$code"; continue }

    foreach ($f in @('host', 'client')) {
        $log = Join-Path $OutDir "$f.log"
        if ((Test-Path $log) -and ((Get-Item $log).Length -gt $MAX_LOG_BYTES)) {
            $failures += "round $round ${f}.log over $MAX_LOG_BYTES bytes (leak?)"
        }
        # Crash/hang signatures. First-chance vectored exceptions and the
        # crash-handler boot line are normal noise — only count fatal events.
        $bad = Select-String -Path $log -Pattern 'second-chance|unhandled|SIGSEGV|abort\(\)|Fatal' -ErrorAction SilentlyContinue
        if ($bad) { $failures += "round $round ${f}.log shows crash signature" }
    }
}

if ($SynctestMinutes -gt 0) {
    Write-Output "=== long synctest ${SynctestMinutes}min distance $SynctestDistance ==="
    & $synctest -BuildDir $BuildDir -Minutes $SynctestMinutes -Distance $SynctestDistance `
        -TimeoutSeconds ([Math]::Max(400, $SynctestMinutes * 70)) | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { $failures += "synctest exit=$LASTEXITCODE" }
}

if ($failures.Count -gt 0) {
    Write-Output "SOAK FAIL:"
    $failures | ForEach-Object { Write-Output "  $_" }
    exit 1
}
Write-Output "SOAK PASS - $RematchRounds rounds$(if ($SynctestMinutes -gt 0) { " + ${SynctestMinutes}min synctest" })"
exit 0
