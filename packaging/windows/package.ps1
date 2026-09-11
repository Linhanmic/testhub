# Stage testhub.exe + specs/runners into dist/testhub-windows (run after a Release build).
param(
    [string]$BuildDir = "",
    [string]$OutDir = "$(Join-Path $PSScriptRoot '..\..\dist\testhub-windows')"
)
$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

function Find-Binary {
    $candidates = @()
    if ($BuildDir) { $candidates += (Join-Path $BuildDir "testhub.exe") }
    $candidates += @(
        (Join-Path $RepoRoot "build\Release\testhub.exe"),
        (Join-Path $RepoRoot "build\testhub.exe"),
        (Join-Path $RepoRoot "out\Release\testhub.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    throw "testhub.exe not found. Build first: cmake --build build --config Release"
}

$exe = Find-Binary
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $exe (Join-Path $OutDir "testhub.exe") -Force
Copy-Item (Join-Path $RepoRoot "specs") (Join-Path $OutDir "specs") -Recurse -Force
Copy-Item (Join-Path $RepoRoot "runners") (Join-Path $OutDir "runners") -Recurse -Force
Copy-Item (Join-Path $PSScriptRoot "testhub.json") (Join-Path $OutDir "testhub.json") -Force
Copy-Item (Join-Path $PSScriptRoot "install-service.ps1") (Join-Path $OutDir "install-service.ps1") -Force
Copy-Item (Join-Path $PSScriptRoot "uninstall-service.ps1") (Join-Path $OutDir "uninstall-service.ps1") -Force
Write-Host "Staged Windows package at $OutDir"
Get-ChildItem $OutDir | Format-Table Name, Length
