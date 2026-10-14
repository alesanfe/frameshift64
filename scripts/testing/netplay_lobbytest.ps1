# Thin wrapper — the test lives in tests/e2e/netplay/lobbytest_test.py.
param(
    [int]$Seconds = 60,
    [int]$LatencyMs = 0,
    [int]$JitterMs = 0,
    [int]$LossPct = 0,
    [string]$Server = "",
    [int]$HostFighter = 0,
    [int]$ClientFighter = 1,
    [int]$Stage = 0,
    [switch]$ForceRelay,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [int]$AddrJitter = 0,
    [string]$HostDir = "",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\lobby-test"
)
$pyArgs = @(
    "$PSScriptRoot\..\..\tests\e2e\netplay\lobbytest_test.py",
    "--seconds", "$Seconds", "--latency-ms", "$LatencyMs",
    "--jitter-ms", "$JitterMs", "--loss-pct", "$LossPct",
    "--server", "$Server", "--host-fighter", "$HostFighter",
    "--client-fighter", "$ClientFighter", "--stage", "$Stage",
    "--addr-jitter", "$AddrJitter",
    "--build-dir", "$BuildDir", "--out-dir", "$OutDir")
if ($ForceRelay) { $pyArgs += "--force-relay" }
if ($HostDir) { $pyArgs += @("--host-dir", "$HostDir") }
python @pyArgs
exit $LASTEXITCODE
