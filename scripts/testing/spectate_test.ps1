# Thin wrapper — the test lives in tests/e2e/netplay/spectate_test.py.
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
$pyArgs = @(
    "$PSScriptRoot\..\..\tests\e2e\netplay\spectate_test.py",
    "--players", "$Players", "--seconds", "$Seconds",
    "--latency-ms", "$LatencyMs", "--jitter-ms", "$JitterMs",
    "--server", "$Server",
    "--build-dir", "$BuildDir", "--out-dir", "$OutDir")
if ($DebugStages) { $pyArgs += "--debug-stages" }
python @pyArgs
exit $LASTEXITCODE
