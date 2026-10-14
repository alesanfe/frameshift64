# Thin wrapper — the test lives in tests/e2e/netplay/lan_test.py.
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
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\lan-test"
)
$pyArgs = @(
    "$PSScriptRoot\..\..\tests\e2e\netplay\lan_test.py",
    "--players", "$Players", "--seconds", "$Seconds", "--port", "$Port",
    "--latency-ms", "$LatencyMs", "--jitter-ms", "$JitterMs",
    "--loss-pct", "$LossPct", "--replay-frames", "$ReplayFrames",
    "--desync-tick", "$DesyncTick",
    "--build-dir", "$BuildDir", "--out-dir", "$OutDir")
if ($DebugStages) { $pyArgs += "--debug-stages" }
python @pyArgs
exit $LASTEXITCODE
