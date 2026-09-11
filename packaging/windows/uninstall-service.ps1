# Stop and delete the TestHub Windows service. Requires Administrator.
#requires -RunAsAdministrator
param(
    [string]$Name = "TestHub"
)
$ErrorActionPreference = "Stop"
$exe = "$env:ProgramFiles\TestHub\testhub.exe"
if (-not (Test-Path $exe)) {
    $cmd = Get-Command testhub -ErrorAction SilentlyContinue
    if ($cmd) { $exe = $cmd.Source }
}
if (Test-Path $exe) {
    & $exe --service uninstall --service-name $Name
} else {
    Stop-Service -Name $Name -ErrorAction SilentlyContinue
    sc.exe delete $Name | Out-Null
}
Write-Host "Service '$Name' removed."
