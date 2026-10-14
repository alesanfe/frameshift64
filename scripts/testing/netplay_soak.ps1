# Thin wrapper — the test lives in tests/e2e/netplay/soak_test.py.
param(
    [int]$RematchRounds = 3,
    [int]$Seconds = 45,
    [int]$SynctestMinutes = 0,
    [int]$SynctestDistance = 3,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\netplay-test",
    [int]$LatencyMs = 50,
    [int]$JitterMs = 15,
    [int]$LossPct = 1,
    [int]$DupPct = 1,
    [int]$ReorderPct = 2
)
python "$PSScriptRoot\..\..\tests\e2e\netplay\soak_test.py" `
    --rematch-rounds $RematchRounds --seconds $Seconds `
    --synctest-minutes $SynctestMinutes --synctest-distance $SynctestDistance `
    --build-dir "$BuildDir" --out-dir "$OutDir" `
    --latency-ms $LatencyMs --jitter-ms $JitterMs --loss-pct $LossPct `
    --dup-pct $DupPct --reorder-pct $ReorderPct
exit $LASTEXITCODE
