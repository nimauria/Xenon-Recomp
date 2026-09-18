[CmdletBinding()]
param(
    [string]$QtRoot = $env:QTDIR,
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",
    [switch]$TestMode,
    [switch]$Clean,
    [switch]$Deploy,
    [switch]$Run,
    [switch]$FullRuntime
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildDir = Join-Path $repoRoot "build\launcher-ui"

function Find-QtRoot {
    param([string]$Requested)

    if ($Requested -and (Test-Path (Join-Path $Requested "lib\cmake\Qt6\Qt6Config.cmake"))) {
        return (Resolve-Path $Requested).Path
    }

    $candidates = @()
    if ($env:CMAKE_PREFIX_PATH) {
        $candidates += ($env:CMAKE_PREFIX_PATH -split ';')
    }
    $candidates += @(
        "C:\Qt\6.10.3\msvc2022_64",
        "C:\Qt\6.10.2\msvc2022_64",
        "C:\Qt\6.10.1\msvc2022_64",
        "C:\Qt\6.10.0\msvc2022_64"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "lib\cmake\Qt6\Qt6Config.cmake"))) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Qt 6 MSVC was not found. Pass -QtRoot C:\Qt\<version>\msvc2022_64 or set QTDIR."
}

$QtRoot = Find-QtRoot $QtRoot
$qtBin = Join-Path $QtRoot "bin"
$env:CMAKE_PREFIX_PATH = $QtRoot
$env:Path = "$qtBin;$env:Path"

foreach ($tool in @("cmake", "ninja", "cl")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required build tool '$tool' is not available in this terminal. Open VS Code from an x64 Visual Studio developer shell."
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

$testModeValue = if ($TestMode) { "ON" } else { "OFF" }
$runtimeValue = if ($FullRuntime) { "ON" } else { "OFF" }

$configureArgs = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DXENON_BUILD_LAUNCHER=ON",
    "-DXENON_LAUNCHER_TEST_MODE=$testModeValue",
    "-DXENON_BUILD_TESTS=OFF",
    "-DXENON_ENABLE_AUDIO=OFF",
    "-DXENON_ENABLE_INPUT=OFF",
    "-DXENON_ENABLE_NETWORK=OFF"
)

if (-not $FullRuntime) {
    $configureArgs += @(
        "-DXENON_ENABLE_MEMORY=OFF",
        "-DXENON_ENABLE_GRAPHICS=OFF",
        "-DXENON_ENABLE_VULKAN=OFF",
        "-DXENON_ENABLE_D3D12=OFF",
        "-DXENON_ENABLE_DXC=OFF"
    )
}

Write-Host "Configuring Xenon Launcher ($Configuration, test mode: $testModeValue)" -ForegroundColor Cyan
& cmake @configureArgs

Write-Host "Building xenon_launcher" -ForegroundColor Cyan
& cmake --build $buildDir --target xenon_launcher -j 8

$exe = Join-Path $buildDir "launcher\xenon_launcher.exe"
if (-not (Test-Path $exe)) {
    throw "Build completed without producing $exe"
}

if ($Deploy) {
    $deployTool = Join-Path $qtBin "windeployqt.exe"
    if (-not (Test-Path $deployTool)) {
        throw "windeployqt.exe was not found under $qtBin"
    }

    $deployMode = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
    Write-Host "Deploying Qt runtime" -ForegroundColor Cyan
    & $deployTool $deployMode --qmldir (Join-Path $repoRoot "launcher\qml") $exe
}

Write-Host "Launcher ready: $exe" -ForegroundColor Green

if ($Run) {
    Write-Host "Starting Xenon Launcher" -ForegroundColor Cyan
    & $exe
}
