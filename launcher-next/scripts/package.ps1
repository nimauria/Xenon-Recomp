param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$launcherPackageRoot = Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot 'desktop.ps1') -BuildOnly
  if ($LASTEXITCODE -ne 0) { throw 'Desktop build failed' }
}
$launcherPackageSource = Join-Path $launcherPackageRoot 'build/native/desktop'
$launcherBuildInfo = Get-Content -LiteralPath (Join-Path $launcherPackageRoot 'build/native/launcher-build.json') -Raw | ConvertFrom-Json
if ($launcherBuildInfo.platform -ne 'windows-x64') { throw 'This packager currently supports the Windows x64 desktop build' }
$launcherPackageOutput = Join-Path $launcherPackageRoot ('build/packages/' + [guid]::NewGuid().ToString('N'))
$launcherPackagePayload = Join-Path $launcherPackageOutput 'payload'
New-Item -ItemType Directory -Path $launcherPackagePayload -Force | Out-Null
$launcherRootFiles = @(
  'xenon_launcher_next.exe', 'xenon_launcher_next.dll', 'xenon_launcher_updater.exe',
  'libcef.dll', 'chrome_elf.dll', 'd3dcompiler_47.dll', 'dxcompiler.dll',
  'vk_swiftshader.dll', 'vk_swiftshader_icd.json', 'vulkan-1.dll',
  'chrome_100_percent.pak', 'chrome_200_percent.pak', 'icudtl.dat',
  'resources.pak', 'v8_context_snapshot.bin', 'settings-schema.json',
  'module-catalog-fallback.json', 'CEF-LICENSE.txt'
)
foreach ($launcherPackageFile in $launcherRootFiles) {
  $launcherInputFile = Join-Path $launcherPackageSource $launcherPackageFile
  $launcherInputItem = Get-Item -LiteralPath $launcherInputFile
  if ($launcherInputItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package input is redirected: $launcherPackageFile" }
  Copy-Item -LiteralPath $launcherInputFile -Destination (Join-Path $launcherPackagePayload $launcherPackageFile)
}
foreach ($launcherPackageDirectory in @('web', 'locales', 'licenses')) {
  $launcherInputDirectory = Join-Path $launcherPackageSource $launcherPackageDirectory
  $launcherInputEntries = @(Get-Item -LiteralPath $launcherInputDirectory) + @(Get-ChildItem -LiteralPath $launcherInputDirectory -Recurse -Force)
  foreach ($launcherInputEntry in $launcherInputEntries) {
    if ($launcherInputEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package input is redirected: $($launcherInputEntry.FullName)" }
  }
  Copy-Item -LiteralPath $launcherInputDirectory -Destination (Join-Path $launcherPackagePayload $launcherPackageDirectory) -Recurse
}
$launcherPackageManifest = [ordered]@{
  product = 'xenon-launcher-next'; format = 1; stateSchema = 1
  version = $launcherBuildInfo.version; platform = $launcherBuildInfo.platform
}
$launcherPackageManifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $launcherPackagePayload 'launcher-package.json') -Encoding UTF8
$launcherPackageArchive = Join-Path $launcherPackageOutput ('xenon-launcher-next-' + $launcherBuildInfo.platform + '.zip')
& (Join-Path $PSScriptRoot 'write-package-archive.ps1') -SourceDirectory $launcherPackagePayload -DestinationArchive $launcherPackageArchive
$launcherPackageDigest = (Get-FileHash -LiteralPath $launcherPackageArchive -Algorithm SHA256).Hash.ToLowerInvariant()
($launcherPackageDigest + '  ' + [IO.Path]::GetFileName($launcherPackageArchive)) | Set-Content -LiteralPath ($launcherPackageArchive + '.sha256') -Encoding ASCII
Write-Output $launcherPackageArchive
Write-Output ('SHA256 ' + $launcherPackageDigest)
Write-Output ('Payload ' + $launcherPackagePayload)
