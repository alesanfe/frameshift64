# Runs one unattended 4-CPU VS match with rollback SyncTest enabled and prints
# the verdict plus the first divergences. See docs/rollback_design.md.
#
#   .\scripts\development\rollback_synctest.ps1 -Stage 0 -Fighters "0,1,2,3" -Seed 12345
#
# Exit code: 0 PASS, 1 FAIL, 2 timeout/crash.
param(
    [int]$Stage = -1,
    [string]$Fighters = "",
    [int]$Seed = 12345,
    [int]$Minutes = 1,
    [int]$Distance = 1,
    # Mutation testing: skip_rng_restore leaves the live RNG seed in place
    # after a snapshot load — SyncTest must FAIL, proving RNG coverage.
    [string]$Mutant = "",
    [int]$TimeoutSeconds = 400,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [int]$ShowLines = 40
)

$env:SSB64_ROLLBACK_SYNCTEST = "$Distance"
$env:SSB64_MUTE = '1'
$env:SSB64_NO_UPDATE_CHECK = '1'
$env:SSB64_ROLLBACK_AUTOVS = '1'
$env:SSB64_RIG_EXIT = '1'
$env:SSB64_ROLLBACK_SEED = "$Seed"
$env:SSB64_ROLLBACK_MINUTES = "$Minutes"
if ($Mutant) { $env:SSB64_ROLLBACK_MUTANT = $Mutant } else { Remove-Item Env:SSB64_ROLLBACK_MUTANT -ErrorAction SilentlyContinue }
if ($Stage -ge 0) { $env:SSB64_ROLLBACK_STAGE = "$Stage" } else { Remove-Item Env:SSB64_ROLLBACK_STAGE -ErrorAction SilentlyContinue }
if ($Fighters) { $env:SSB64_ROLLBACK_FIGHTERS = $Fighters } else { Remove-Item Env:SSB64_ROLLBACK_FIGHTERS -ErrorAction SilentlyContinue }

$exe = Join-Path (Resolve-Path $BuildDir) 'BattleShip.exe'
$p = Start-Process -FilePath $exe -WorkingDirectory (Resolve-Path $BuildDir) -PassThru
if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process $p -Force
    Write-Output "TIMEOUT after $TimeoutSeconds s"
    $code = 2
} else {
    $code = $p.ExitCode
}

$log = Join-Path (Resolve-Path $BuildDir) 'logs\ssb64.log'
if (-not (Test-Path $log)) { $log = Join-Path $env:APPDATA 'BattleShip\ssb64.log' }
Select-String -Path $log -Pattern 'SSB64 Rollback' | Select-Object -First $ShowLines | ForEach-Object { $_.Line }
Select-String -Path $log -Pattern 'SyncTest (PASS|FAIL)' | ForEach-Object { $_.Line }
if ($code -ne 0 -and $code -ne 1) {
    Select-String -Path $log -Pattern 'crash|Exception|SIGSEGV|abort' | Select-Object -Last 10 | ForEach-Object { $_.Line }
}
Write-Output "exit=$code"
exit $code
