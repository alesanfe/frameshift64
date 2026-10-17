# package-uwp.ps1 — Produce the UWP MSIX package.
#
# Delegates to platforms/uwp/ci/build-uwp.ps1, which configures the
# BATTLESHIP_UWP native build and then the appx packaging project
# (Package.appxmanifest + Assets live under platforms/uwp/).
#
#   .\packaging\uwp\package-uwp.ps1 [-PackageVersion 1.0.0.0] [-SigningPfx <path>]

param(
    [string]$PackageVersion = "1.0.0.0",
    [string]$SigningPfx = "",
    [string]$SigningPassword = "",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if ($Output -eq "") {
    $Output = Join-Path $repo "out\dist\uwp"
}

& (Join-Path $repo "platforms\uwp\ci\build-uwp.ps1") `
    -Source $repo `
    -Output $Output `
    -PackageVersion $PackageVersion `
    -SigningPfx $SigningPfx `
    -SigningPassword $SigningPassword
