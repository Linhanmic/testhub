# Copy the staged package into Program Files and register a Windows service.
# Requires Administrator. Run from the staged dist folder or this repo's packaging/windows.
#requires -RunAsAdministrator
param(
    [string]$InstallDir = "$env:ProgramFiles\TestHub",
    [string]$Name = "TestHub",
    [string]$SourceDir = ""
)
$ErrorActionPreference = "Stop"

if (-not $SourceDir) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    if (Test-Path (Join-Path $here "testhub.exe")) {
        $SourceDir = $here
    } else {
        $SourceDir = Join-Path $here "..\..\dist\testhub-windows"
    }
}
$SourceDir = (Resolve-Path $SourceDir).Path
$exeSrc = Join-Path $SourceDir "testhub.exe"
if (-not (Test-Path $exeSrc)) { throw "testhub.exe missing in $SourceDir (run package.ps1 first)" }

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item (Join-Path $SourceDir "*") $InstallDir -Recurse -Force
$data = Join-Path $InstallDir "data"
New-Item -ItemType Directory -Force -Path (Join-Path $data "results"), (Join-Path $data "schedules") | Out-Null

$exe = Join-Path $InstallDir "testhub.exe"
$config = Join-Path $InstallDir "testhub.json"
& $exe --service install --service-name $Name --config $config
if ($LASTEXITCODE -ne 0) { throw "testhub --service install failed with exit $LASTEXITCODE" }
Start-Service -Name $Name
Write-Host "Service '$Name' installed at $InstallDir and started."
Write-Host "UI: http://localhost:8080/"
