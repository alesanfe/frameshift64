# Thin wrapper — the test lives in tests/e2e/netplay/lobbytest_multi_test.py
# (driven by tests/harness/netplay/session_runner.py). This script only maps
# the historical parameters.
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
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\lobby-test-multi"
)
$pyArgs = @(
    "$PSScriptRoot\..\..\tests\e2e\netplay\lobbytest_multi_test.py",
    "--players", "$Players", "--seconds", "$Seconds",
    "--latency-ms", "$LatencyMs", "--jitter-ms", "$JitterMs",
    "--loss-pct", "$LossPct", "--server", "$Server", "--stage", "$Stage",
    "--build-dir", "$BuildDir", "--out-dir", "$OutDir")
if ($ForceRelay) { $pyArgs += "--force-relay" }
if ($DebugStages) { $pyArgs += "--debug-stages" }
python @pyArgs
exit $LASTEXITCODE
