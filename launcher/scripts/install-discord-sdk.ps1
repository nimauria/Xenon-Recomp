[CmdletBinding(DefaultParameterSetName = "Archive")]
param(
    [Parameter(Mandatory = $true, ParameterSetName = "Archive")]
    [string]$Archive,

    [Parameter(Mandatory = $true, ParameterSetName = "Directory")]
    [string]$SourceDirectory,

    [string]$Destination,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\\..")).Path
if (-not $Destination) {
    $Destination = Join-Path $repoRoot "launcher\\third_party\\discord_social_sdk"
}

function Find-SdkRoot {
    param([string]$Root)

    $header = Get-ChildItem -Path $Root -Filter "discordpp.h" -File -Recurse | Select-Object -First 1
    if (-not $header) {
        throw "Could not find discordpp.h inside $Root"
    }

    $includeDir = $header.Directory
    if ($includeDir.Name -ne "include") {
        throw "Found discordpp.h at $($header.FullName), but it is not inside the expected include directory."
    }

    $sdkRoot = $includeDir.Parent.FullName
    $required = @(
        (Join-Path $sdkRoot "include\\discordpp.h"),
        (Join-Path $sdkRoot "lib\\release\\discord_partner_sdk.lib"),
        (Join-Path $sdkRoot "bin\\release\\discord_partner_sdk.dll")
    )
    foreach ($path in $required) {
        if (-not (Test-Path $path)) {
            throw "Discord Social SDK package is incomplete. Missing: $path"
        }
    }
    return $sdkRoot
}

$tempDir = $null
try {
    if ($PSCmdlet.ParameterSetName -eq "Archive") {
        if (-not (Test-Path $Archive)) { throw "Archive not found: $Archive" }
        $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("xenon-discord-sdk-" + [guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $tempDir | Out-Null
        Write-Host "Extracting Discord Social SDK..." -ForegroundColor Cyan
        Expand-Archive -Path (Resolve-Path $Archive).Path -DestinationPath $tempDir -Force
        $sdkRoot = Find-SdkRoot $tempDir
    } else {
        if (-not (Test-Path $SourceDirectory)) { throw "Source directory not found: $SourceDirectory" }
        $sdkRoot = Find-SdkRoot (Resolve-Path $SourceDirectory).Path
    }

    if (Test-Path $Destination) {
        if (-not $Force) {
            throw "Destination already exists: $Destination. Re-run with -Force to replace it."
        }
        Remove-Item -Recurse -Force $Destination
    }

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Write-Host "Installing Discord Social SDK to $Destination" -ForegroundColor Cyan
    Copy-Item -Path (Join-Path $sdkRoot "*") -Destination $Destination -Recurse -Force

    $installed = Find-SdkRoot $Destination
    Write-Host "Discord Social SDK installed successfully." -ForegroundColor Green
    Write-Host "Root: $installed"
    Write-Host "The normal launcher build script will detect this location automatically."
    Write-Host "Example: .\\launcher\\scripts\\build-launcher.ps1 -Deploy -Run -RequireDiscordSdk"
} finally {
    if ($tempDir -and (Test-Path $tempDir)) {
        Remove-Item -Recurse -Force $tempDir
    }
}
