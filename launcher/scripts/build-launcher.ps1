[CmdletBinding()]
param(
    [string]$QtRoot = $env:QTDIR,
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Configuration = "RelWithDebInfo",
    [switch]$TestMode,
    [switch]$Clean,
    [switch]$Deploy,
    [switch]$Run,
    [switch]$SafeMode,
    [switch]$StartMinimized,
    [string[]]$LauncherArguments = @(),
    [switch]$FullRuntime,
    [string]$DiscordSdkRoot = $env:DISCORD_SOCIAL_SDK_ROOT,
    [switch]$EnableDiscordRichPresence,
    [switch]$RequireDiscordSdk
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

function Find-DiscordSdkRoot {
    param([string]$Requested)

    $candidates = @()
    if ($Requested) { $candidates += $Requested }
    $candidates += @(
        (Join-Path $repoRoot "launcher\third_party\discord_social_sdk"),
        (Join-Path $repoRoot "third_party\discord_social_sdk")
    )

    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        $header = Join-Path $candidate "include\discordpp.h"
        if (Test-Path $header) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

$QtRoot = Find-QtRoot $QtRoot
if ($RequireDiscordSdk) { $EnableDiscordRichPresence = $true }
$DiscordSdkRoot = if ($EnableDiscordRichPresence) { Find-DiscordSdkRoot $DiscordSdkRoot } else { $null }
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
    "-DXENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE=$(if ($EnableDiscordRichPresence) { 'ON' } else { 'OFF' })",
    "-DXENON_BUILD_TESTS=OFF",
    "-DXENON_ENABLE_AUDIO=OFF",
    "-DXENON_ENABLE_INPUT=OFF",
    "-DXENON_ENABLE_NETWORK=OFF"
)

if ($EnableDiscordRichPresence) {
    if ($DiscordSdkRoot) {
        Write-Host "Discord Social SDK: $DiscordSdkRoot" -ForegroundColor Cyan
        $configureArgs += "-DXENON_DISCORD_SOCIAL_SDK_ROOT=$DiscordSdkRoot"
    } elseif ($RequireDiscordSdk) {
        throw "Discord Social SDK was required but was not found. Run launcher\scripts\install-discord-sdk.ps1 -Archive <sdk.zip>, pass -DiscordSdkRoot <path>, or set DISCORD_SOCIAL_SDK_ROOT."
    } else {
        Write-Host "Discord Rich Presence requested, but the optional Social SDK was not found." -ForegroundColor DarkYellow
    }

    if ($RequireDiscordSdk) {
        $configureArgs += "-DXENON_LAUNCHER_REQUIRE_DISCORD_SOCIAL_SDK=ON"
    }
} else {
    Write-Host "Discord Rich Presence disabled; Discord community/support links remain available." -ForegroundColor DarkGray
}


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

if ($EnableDiscordRichPresence -and $DiscordSdkRoot) {
    $discordRuntime = Join-Path (Split-Path $exe -Parent) "discord_partner_sdk.dll"
    if (Test-Path $discordRuntime) {
        Write-Host "Discord Rich Presence runtime deployed: $discordRuntime" -ForegroundColor Green
    } elseif ($RequireDiscordSdk) {
        throw "Discord Social SDK was configured, but discord_partner_sdk.dll was not deployed beside the launcher."
    } else {
        Write-Host "Discord SDK was detected, but discord_partner_sdk.dll was not deployed beside the launcher." -ForegroundColor Yellow
    }
}

if ($Deploy) {
    # Kept for command-line compatibility. Qt/QML deployment is now owned by
    # the xenon_launcher CMake target itself, so direct CMake/Visual Studio
    # builds and this helper script produce the same runnable output.
    Write-Host "Qt runtime deployment is automatic for xenon_launcher builds; -Deploy is no longer required." -ForegroundColor DarkGray
}

Write-Host "Launcher ready: $exe" -ForegroundColor Green

if ($Run) {
    Write-Host "Starting Xenon Launcher" -ForegroundColor Cyan
    $runArgs = @($LauncherArguments)
    if ($SafeMode) {
        $runArgs += "--safe-mode"
        Write-Host "Safe Mode requested for this launcher run" -ForegroundColor Yellow
    }
    if ($StartMinimized) {
        $runArgs += "--start-minimized"
        Write-Host "Start minimized requested for this launcher run" -ForegroundColor DarkGray
    }
    if ($runArgs.Count -gt 0) {
        $process = Start-Process -FilePath $exe -ArgumentList $runArgs -PassThru
    } else {
        $process = Start-Process -FilePath $exe -PassThru
    }
    $process.WaitForExit()
    Write-Host "Xenon Launcher exited with code $($process.ExitCode)" -ForegroundColor $(if ($process.ExitCode -eq 0) { "Green" } else { "Yellow" })

    $startupLog = Join-Path $env:LOCALAPPDATA "Project Xenon\Xenon Launcher\xenon-launcher-startup.log"
    if ($process.ExitCode -ne 0 -and (Test-Path $startupLog)) {
        Write-Host "Startup log: $startupLog" -ForegroundColor Yellow
        Get-Content $startupLog -Tail 80
    }
}
