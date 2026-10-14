# Thin wrapper — the test lives in tests/e2e/netplay/replay_takeover_test.py.
param(
    [Parameter(Mandatory = $true)][string]$Replay,
    [string]$Takeover = "",
    [int]$Seconds = 20,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\takeover-test"
)
$pyArgs = @(
    "$PSScriptRoot\..\..\tests\e2e\netplay\replay_takeover_test.py",
    "--replay", "$Replay", "--takeover", "$Takeover",
    "--seconds", "$Seconds",
    "--build-dir", "$BuildDir", "--out-dir", "$OutDir")
python @pyArgs
exit $LASTEXITCODE
