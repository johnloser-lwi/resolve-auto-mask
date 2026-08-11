# Build AutoMask.ofx.bundle
#
# Uses the CMake bundled with Visual Studio: the standalone CMake on PATH may be
# older than the installed VS toolset and will not know its generator. Measured
# on this machine -- system CMake 3.28 has no "Visual Studio 18 2026" generator,
# while the copy inside VS 18 (4.3.1) does.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }

$vsPath  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio install with the C++ toolset was found." }
$vsMajor = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

$yearMap = @{ '17' = '2022'; '18' = '2026' }
$year = $yearMap[$vsMajor]
if (-not $year) { throw "Unrecognised Visual Studio major version '$vsMajor'. Add it to `$yearMap." }
$generator = "Visual Studio $vsMajor $year"

$buildDir = Join-Path $repo 'build'
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "CMake:     $cmake"
Write-Host "Generator: $generator"

& $cmake -S $repo -B $buildDir -G $generator -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host ""
Write-Host "Bundle: $(Join-Path $repo "build\$Config\AutoMask.ofx.bundle")"

