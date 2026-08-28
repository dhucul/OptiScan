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
$stagingDir = Join-Path $PSScriptRoot 'Release'
New-Item -ItemType Directory -Path $stagingDir -Force | Out-Null

# --- Locate MSBuild via vswhere (portable across VS versions) ---------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
    Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found via vswhere." }

# --- Locate ISCC (Inno Setup): newest major first, per-user before machine ---
# Enumerated rather than hard-coded to one major version: the pinned "6" list
# silently stopped finding the compiler when Inno Setup 7 was installed.
# Note the quoted form for the first entry: written as $env:LOCALAPPDATA +
# "\Programs", ... the + binds against the whole comma list and folds
# all three roots into a single string.
$isccRoots = @("$env:LOCALAPPDATA\Programs", ${env:ProgramFiles(x86)}, $env:ProgramFiles) |
    Where-Object { $_ -and (Test-Path $_) }
$iscc = $isccRoots |
    ForEach-Object { Get-ChildItem -Path $_ -Filter "Inno Setup *" -Directory -ErrorAction SilentlyContinue } |
    ForEach-Object {
        $exe = Join-Path $_.FullName "ISCC.exe"
        if (Test-Path $exe) {
            $major = 0
            if ($_.Name -match "(\d+)$") { $major = [int]$Matches[1] }
            [pscustomobject]@{ Path = $exe; Major = $major }
        }
    } |
    Sort-Object Major -Descending |
    Select-Object -First 1 -ExpandProperty Path
if (-not $iscc) { throw "ISCC.exe (Inno Setup) not found in Program Files or %LOCALAPPDATA%\Programs." }

Write-Host "=== Building Release | x64 ===" -ForegroundColor Cyan
Write-Host "  MSBuild: $msbuild"
Write-Host "  Staging: $stagingDir"
# Build away from x64\Release so packaging still works when a developer is
# running that executable and Windows has it locked against relinking.
& $msbuild OptiScan.slnx -p:Configuration=Release -p:Platform=x64 `
    "-p:OutDir=$stagingDir\" -m -nologo -v:m
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "`n=== Compiling installer ===" -ForegroundColor Cyan
Write-Host "  ISCC: $iscc"
& $iscc "/DReleaseDir=$stagingDir" installer\OptiScan.iss
if ($LASTEXITCODE -ne 0) { throw "Installer compile failed." }

Write-Host "`n=== Done. Installer written to installer\Output\ ===" -ForegroundColor Green
Get-ChildItem installer\Output\*.exe | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
