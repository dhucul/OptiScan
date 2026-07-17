# ============================================================================
#  OptiScan - build Release x64 and package the Inno Setup installer.
#
#  Run manually:  right-click build-installer.bat > Run,  or from a terminal:
#      powershell -ExecutionPolicy Bypass -File build-installer.ps1
#
#  Before a release, bump the version in THREE places, then run this:
#      - installer\OptiScan.iss   ( #define MyAppVersion "x.y" )
#      - OptiScan.rc              ( "OptiScan, Version x.y" in the About box )
#      - UpdateChecker.cpp        ( const VersionInfo APP_VERSION = { x, y, 0 } )
#  The output filename comes from MyAppVersion in the .iss.
#  Produces:  installer\Output\OptiScan-<version>-Setup.exe
# ============================================================================
$ErrorActionPreference = 'Stop'
# This script lives in installer\; the solution is one level up at the repo root.
Set-Location (Split-Path -Parent $PSScriptRoot)

# --- Locate MSBuild via vswhere (portable across VS versions) ---------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
    Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found via vswhere." }

# --- Locate ISCC (Inno Setup 6): per-user install first, then Program Files --
$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe (Inno Setup 6) not found." }

Write-Host "=== Building Release | x64 ===" -ForegroundColor Cyan
Write-Host "  MSBuild: $msbuild"
& $msbuild OptiScan.slnx -p:Configuration=Release -p:Platform=x64 -m -nologo -v:m
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "`n=== Compiling installer ===" -ForegroundColor Cyan
Write-Host "  ISCC: $iscc"
& $iscc installer\OptiScan.iss
if ($LASTEXITCODE -ne 0) { throw "Installer compile failed." }

Write-Host "`n=== Done. Installer written to installer\Output\ ===" -ForegroundColor Green
Get-ChildItem installer\Output\*.exe | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
