# Thin wrapper — the test lives in tests/e2e/netplay/mutation_suite_test.py.
param(
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\netplay-test",
    [int]$Minutes = 1,
    [int]$Seconds = 45
)
python "$PSScriptRoot\..\..\tests\e2e\netplay\mutation_suite_test.py" `
    --build-dir "$BuildDir" --out-dir "$OutDir" `
    --minutes $Minutes --seconds $Seconds
exit $LASTEXITCODE
