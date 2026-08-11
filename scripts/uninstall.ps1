# Remove AutoMask.ofx.bundle from the system OFX plugin directory.

[CmdletBinding()]
param(
    [string]$Destination = 'C:\Program Files\Common Files\OFX\Plugins',
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'
$target = Join-Path $Destination 'AutoMask.ofx.bundle'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path $target)) {
    Write-Host "Nothing to remove - $target does not exist."
    exit 0
}

if (-not $isAdmin) {
    Write-Host "==> Requesting elevation to remove $target" -ForegroundColor Cyan
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                 '-File', "`"$PSCommandPath`"",
                 '-Destination', "`"$Destination`"", '-Elevated')
    try {
        $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -PassThru -Wait
    } catch {
        Write-Host "Elevation declined. Nothing was removed." -ForegroundColor Yellow
        exit 1
    }
    exit $proc.ExitCode
}

try {
    if (Get-Process -Name 'Resolve' -ErrorAction SilentlyContinue) {
        Write-Warning "DaVinci Resolve is running and may hold a lock on the plugin."
    }
    Remove-Item -Recurse -Force $target -ErrorAction Stop
    Write-Host "Removed $target" -ForegroundColor Green
    Write-Host "Restart Resolve for the change to take effect."
    $exitCode = 0
} catch {
    Write-Host "UNINSTALL FAILED" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    $exitCode = 1
}

if ($Elevated) { Read-Host "Press Enter to close" }
exit $exitCode

