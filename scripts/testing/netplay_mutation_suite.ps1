# Mutation suite (testplan s.21): each mutant removes exactly one defense,
# then a probe stream hits the hole. Every mutated run must FAIL - a mutant
# that still passes means the check wasn't load-bearing.
#
#   .\scripts\testing\netplay_mutation_suite.ps1 -BuildDir build\us\Release
#
# Mutants:
#   skip_rng_restore   - SyncTest: snapshot load skips RNG state -> resim must diverge
#   accept_bad_session - localtest: wrong-session packets (valid checksum) are
#                        probed into the drain -> must desync, not pass
#   accept_bad_checksum- localtest: checksum-broken packets probed -> must desync
#   no_forward_guard   - localtest: FORWARD datagrams off the host link probed
#                        -> must desync (relay topology check is load-bearing)
#
# Exit: 0 every mutant detected, 1 at least one went undetected, 2 harness error.
param(
    [string]$BuildDir = "$PSScriptRoot\..\..\build\us\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\build\us\netplay-test",
    [int]$Minutes = 1,
    [int]$Seconds = 45
)

$root = Resolve-Path "$PSScriptRoot\..\.."
$synctest = Join-Path $root 'scripts\development\rollback_synctest.ps1'
$localtest = Join-Path $root 'scripts\testing\netplay_localtest.ps1'
$failed = @()

function Expect-Fail([string]$name, [scriptblock]$run) {
    $code = & $run
    if ($code -eq 1) {
        Write-Output "PASS mutant=$name (detected: exit 1)"
    } elseif ($code -eq 0) {
        Write-Output "FAIL mutant=$name went UNDETECTED (exit 0)"
        $script:failed += $name
    } else {
        Write-Output "FAIL mutant=$name harness error (exit $code)"
        $script:failed += $name
    }
}

# Control runs first: the same harness without a mutant must pass, otherwise
# the mutant result is meaningless.
& $synctest -BuildDir $BuildDir -Minutes $Minutes -Distance 1 | Select-Object -Last 1 | ForEach-Object { "baseline synctest> $_" }
if ($LASTEXITCODE -ne 0) { Write-Output "baseline synctest broken (exit $LASTEXITCODE)"; exit 2 }
& $localtest -BuildDir $BuildDir -OutDir $OutDir -Seconds $Seconds | Select-Object -Last 1 | ForEach-Object { "baseline localtest> $_" }
if ($LASTEXITCODE -ne 0) { Write-Output "baseline localtest broken (exit $LASTEXITCODE)"; exit 2 }

Expect-Fail "skip_rng_restore" { & $synctest -BuildDir $BuildDir -Minutes $Minutes -Distance 1 -Mutant skip_rng_restore; $LASTEXITCODE }
Expect-Fail "accept_bad_session" { & $localtest -BuildDir $BuildDir -OutDir $OutDir -Seconds $Seconds -Mutant accept_bad_session; $LASTEXITCODE }
Expect-Fail "accept_bad_checksum" { & $localtest -BuildDir $BuildDir -OutDir $OutDir -Seconds $Seconds -Mutant accept_bad_checksum; $LASTEXITCODE }
Expect-Fail "no_forward_guard" { & $localtest -BuildDir $BuildDir -OutDir $OutDir -Seconds $Seconds -Mutant no_forward_guard; $LASTEXITCODE }

if ($failed.Count -gt 0) {
    Write-Output "MUTATION SUITE FAIL - undetected: $($failed -join ', ')"
    exit 1
}
Write-Output "MUTATION SUITE PASS - all mutants detected"
exit 0
