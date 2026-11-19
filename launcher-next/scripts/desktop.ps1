param([switch]$BuildOnly, [string]$CefRoot, [string]$Version=$env:XENON_NEXT_VERSION, [string]$RuntimeHost=$env:XENON_RUNTIME_HOST)
$ErrorActionPreference = 'Stop'
$launcherRoot = Split-Path $PSScriptRoot -Parent
if (-not $CefRoot) {
  $cefPointer = Join-Path $launcherRoot '.tools/cef-root.txt'
  if (Test-Path -LiteralPath $cefPointer) { $CefRoot = (Get-Content -LiteralPath $cefPointer -Raw).Trim() }
}
if (-not $CefRoot -or -not (Test-Path -LiteralPath (Join-Path $CefRoot 'cmake/FindCEF.cmake'))) {
  throw 'CEF SDK missing. Run npm run cef:fetch, or pass -CefRoot with an extracted distribution.'
}
$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vsWhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$vsRoot = (& $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $vsRoot) { throw 'No Visual Studio installation with C++ build tools was found.' }
$vsDeveloper = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
if (Test-Path -LiteralPath $vsDeveloper) {
  $environmentLines = & cmd.exe /d /s /c "`"$vsDeveloper`" -arch=x64 -host_arch=x64 >nul && set"
  foreach ($line in $environmentLines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') } }
}
$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $cmake) { $cmake = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' }
$ninja = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$nodeDirectory = 'C:/Program Files/nodejs'
if (Test-Path -LiteralPath $nodeDirectory) { $env:PATH = $nodeDirectory + ';' + $env:PATH }
Push-Location $launcherRoot
try {
  & npm.cmd run build
  if ($LASTEXITCODE -ne 0) { throw 'Web build failed' }
  $buildDirectory = Join-Path $launcherRoot 'build/native'
  $versionArgs = @()
  if ($Version) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Launcher version must have the form 0.1.0' }
    $versionArgs = @("-DXENON_NEXT_VERSION=$Version")
  }
  & $cmake -S $launcherRoot -B $buildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release -DXENON_NEXT_BUILD_DESKTOP=ON "-DCEF_ROOT=$CefRoot" "-DCMAKE_MAKE_PROGRAM=$ninja" @versionArgs
  if ($LASTEXITCODE -ne 0) { throw 'Desktop configure failed' }
  & $cmake --build $buildDirectory --parallel 2
  if ($LASTEXITCODE -ne 0) { throw 'Desktop build failed. Close any running launcher before rebuilding.' }
  if (-not $BuildOnly) {
    $launcherRuntimeArguments = @()
    if($RuntimeHost) { $launcherRuntimeArguments += "--runtime-host=$RuntimeHost" }
    & (Join-Path $buildDirectory 'desktop/xenon_launcher_next.exe') @launcherRuntimeArguments
  }
} finally { Pop-Location }
