param(
    [string]$OutputRoot = "",
    [switch]$KeepWorktree
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$XeniaRevision = "95a5c3ee250f80c3b9d139658649d9ffb6db3eec"
$FFmpegRevision = "15ece0882e8d5875051ff5b73c5a8326f7cee9f5"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot "third_party\xenon-ffmpeg\windows-x64"
}

foreach ($Tool in @("git", "python", "msbuild")) {
    if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) {
        throw "Required tool '$Tool' is not available on PATH. Run this from a Visual Studio Developer PowerShell."
    }
}

$WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("xenon-ffmpeg-" + [Guid]::NewGuid().ToString("N"))
$XeniaRoot = Join-Path $WorkRoot "xenia"
New-Item -ItemType Directory -Force $WorkRoot | Out-Null

try {
    Write-Host "Cloning pinned Xenia build harness $XeniaRevision..."
    git clone --quiet https://github.com/xenia-project/xenia.git $XeniaRoot
    Push-Location $XeniaRoot
    git checkout --quiet $XeniaRevision
    git submodule update --init --recursive

    $ActualFFmpegRevision = (git -C (Join-Path $XeniaRoot "third_party\FFmpeg") rev-parse HEAD).Trim()
    if ($ActualFFmpegRevision -ne $FFmpegRevision) {
        throw "Pinned Xenia revision resolved FFmpeg $ActualFFmpegRevision, expected $FFmpegRevision."
    }

    # Xenia's older build wrapper interprets VS 2026 productLineVersion 18 as
    # older than VS2017. Retarget only the temporary build harness to VS2022
    # project format; the installed VS2026 compiler remains in use.
    $BuildScript = Join-Path $XeniaRoot "xenia-build"
    $Text = Get-Content $BuildScript -Raw
    if ($Text -notmatch "version == 18") {
        $Pattern = '(?m)^(\s*install_path = vswhere\[0\]\.get\("installationPath", None\)\s*)$'
        $Replacement = @'
$1

        # Xenon dependency rebuild compatibility: VS 2026 reports version 18.
        if version == 18:
            version = 2022
'@
        $Patched = [regex]::Replace($Text, $Pattern, $Replacement, 1)
        if ($Patched -eq $Text) {
            throw "Could not patch the temporary Xenia Visual Studio detection block."
        }
        Set-Content -Path $BuildScript -Value $Patched -Encoding UTF8
    }

    .\xb setup
    if ($LASTEXITCODE -ne 0) { throw "Xenia dependency setup failed." }

    $VsWhereJson = & .\tools\vswhere\vswhere.exe -version "[15,)" -latest -prerelease -format json -utf8 -products Microsoft.VisualStudio.Product.Enterprise Microsoft.VisualStudio.Product.Professional Microsoft.VisualStudio.Product.Community Microsoft.VisualStudio.Product.BuildTools
    $VsInfo = ($VsWhereJson | ConvertFrom-Json | Select-Object -First 1)
    if (-not $VsInfo) { throw "Visual Studio Build Tools were not found." }
    $VsLine = [int]$VsInfo.catalog.productLineVersion
    $PlatformToolset = switch ($VsLine) {
        18 { "v145" }
        17 { "v143" }
        16 { "v142" }
        15 { "v141" }
        default { throw "Unsupported Visual Studio product line $VsLine." }
    }

    Write-Host "Building libavutil/libavcodec with $PlatformToolset..."
    foreach ($Project in @("libavutil", "libavcodec")) {
        $Args = @(
            (Join-Path $XeniaRoot "build\$Project.vcxproj"),
            "/nologo", "/m", "/v:m",
            "/p:Configuration=Release Windows",
            "/p:Platform=x64",
            "/p:PlatformToolset=$PlatformToolset"
        )
        if ($Project -eq "libavcodec") { $Args += "/p:BuildProjectReferences=true" }
        & msbuild @Args
        if ($LASTEXITCODE -ne 0) { throw "$Project build failed." }
    }

    $BinRoot = Join-Path $XeniaRoot "build\bin\Windows\Release"
    $AvUtilLib = Join-Path $BinRoot "libavutil.lib"
    $AvCodecLib = Join-Path $BinRoot "libavcodec.lib"
    if (-not (Test-Path $AvUtilLib) -or -not (Test-Path $AvCodecLib)) {
        throw "Expected FFmpeg archives were not produced in $BinRoot."
    }

    Remove-Item $OutputRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $OutputRoot "include\libavcodec") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $OutputRoot "include\libavutil") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $OutputRoot "lib") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $OutputRoot "licenses") | Out-Null

    function Copy-PublicHeaders([string]$Library) {
        $Source = Join-Path $XeniaRoot "third_party\FFmpeg\$Library"
        $Makefile = Get-Content (Join-Path $Source "Makefile")
        $Headers = [System.Collections.Generic.List[string]]::new()
        $Collecting = $false
        foreach ($Line in $Makefile) {
            if (-not $Collecting -and $Line -match '^HEADERS\s*=\s*(.*)$') {
                $Collecting = $true
                $Rest = $Matches[1]
            } elseif ($Collecting) {
                $Rest = $Line.Trim()
            } else {
                continue
            }
            $Continues = $Rest.TrimEnd().EndsWith('\')
            $Rest = $Rest.Trim().TrimEnd('\').Trim()
            if ($Rest) {
                foreach ($Header in ($Rest -split '\s+')) { if ($Header) { $Headers.Add($Header) } }
            }
            if (-not $Continues) { break }
        }
        if ($Library -eq "libavutil") {
            $Headers.Add("avconfig.h")
            $Headers.Add("ffversion.h")
        }
        foreach ($Header in ($Headers | Sort-Object -Unique)) {
            $SourceHeader = Join-Path $Source $Header
            $DestHeader = Join-Path (Join-Path $OutputRoot "include\$Library") $Header
            New-Item -ItemType Directory -Force (Split-Path $DestHeader -Parent) | Out-Null
            Copy-Item $SourceHeader $DestHeader -Force
        }
    }

    Copy-PublicHeaders "libavcodec"
    Copy-PublicHeaders "libavutil"
    Copy-Item $AvCodecLib (Join-Path $OutputRoot "lib\libavcodec.lib") -Force
    Copy-Item $AvUtilLib (Join-Path $OutputRoot "lib\libavutil.lib") -Force
    Copy-Item (Join-Path $XeniaRoot "third_party\FFmpeg\LICENSE.md") (Join-Path $OutputRoot "licenses\LICENSE.md") -Force
    Copy-Item (Join-Path $XeniaRoot "third_party\FFmpeg\COPYING.LGPLv2.1") (Join-Path $OutputRoot "licenses\COPYING.LGPLv2.1") -Force

    $CodecId = Join-Path $OutputRoot "include\libavcodec\codec_id.h"
    if (-not (Select-String -Path $CodecId -Pattern "AV_CODEC_ID_XMAFRAMES" -Quiet)) {
        throw "Rebuilt dependency does not expose AV_CODEC_ID_XMAFRAMES."
    }

    $CodecHash = (Get-FileHash (Join-Path $OutputRoot "lib\libavcodec.lib") -Algorithm SHA256).Hash.ToLowerInvariant()
    $UtilHash = (Get-FileHash (Join-Path $OutputRoot "lib\libavutil.lib") -Algorithm SHA256).Hash.ToLowerInvariant()
    $CodecIdHash = (Get-FileHash $CodecId -Algorithm SHA256).Hash.ToLowerInvariant()
    $LicenseHash = (Get-FileHash (Join-Path $OutputRoot "licenses\LICENSE.md") -Algorithm SHA256).Hash.ToLowerInvariant()
    @"
$CodecHash  lib/libavcodec.lib
$UtilHash  lib/libavutil.lib
$CodecIdHash  include/libavcodec/codec_id.h
$LicenseHash  licenses/LICENSE.md
"@ | Set-Content (Join-Path $OutputRoot "SHA256SUMS.txt") -Encoding UTF8
    @"
# Generated identity manifest for the vetted Xenon Windows x64 FFmpeg bundle.
set(XENON_FFMPEG_BUNDLE_AVCODEC_SHA256 "$CodecHash")
set(XENON_FFMPEG_BUNDLE_AVUTIL_SHA256 "$UtilHash")
set(XENON_FFMPEG_BUNDLE_CODEC_ID_SHA256 "$CodecIdHash")
"@ | Set-Content (Join-Path $OutputRoot "XenonFFmpegBundle.cmake") -Encoding UTF8

    Write-Host "Xenon FFmpeg/XMA bundle rebuilt successfully at: $OutputRoot"
} finally {
    Pop-Location -ErrorAction SilentlyContinue
    if ($KeepWorktree) {
        Write-Host "Keeping temporary build tree: $WorkRoot"
    } else {
        Remove-Item $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
