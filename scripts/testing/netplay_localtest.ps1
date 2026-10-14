# Thin wrapper — the test lives in tests/e2e/netplay/localtest_test.py.
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
    [string]$Mutant = "",
    [int]$PacketFuzz = 0,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\netplay-test"
)
python "$PSScriptRoot\..\..\tests\e2e\netplay\localtest_test.py" `
    --seconds $Seconds --delay $Delay --latency-ms $LatencyMs `
    --jitter-ms $JitterMs --loss-pct $LossPct --dup-pct $DupPct `
    --reorder-pct $ReorderPct --reorder-window-ms $ReorderWindowMs `
    --corrupt-pct $CorruptPct --seed $Seed --base-port $BasePort `
    --inject-desync $InjectDesync --mutant "$Mutant" `
    --packet-fuzz $PacketFuzz --build-dir "$BuildDir" --out-dir "$OutDir"
exit $LASTEXITCODE
