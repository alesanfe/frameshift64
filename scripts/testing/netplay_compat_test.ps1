# Thin wrapper — the test lives in tests/e2e/netplay/compat_test.py.
param(
    [switch]$Legacy,
    [int]$Port = 47500,
    [string]$BuildDir = "$PSScriptRoot\..\..\out\build\windows-release\Release",
    [string]$OutDir = "$PSScriptRoot\..\..\out\logs\compat-test"
)
$pyArgs = @("$PSScriptRoot\..\..\tests\e2e\netplay\compat_test.py",
    "--port", "$Port", "--build-dir", "$BuildDir", "--out-dir", "$OutDir")
if ($Legacy) { $pyArgs += "--legacy" }
python @pyArgs
exit $LASTEXITCODE
