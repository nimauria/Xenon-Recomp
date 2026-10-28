param([switch]$BuildOnly, [string]$CefRoot)
$ErrorActionPreference = 'Stop'
$launcherRoot = Split-Path $PSScriptRoot -Parent
if (-not $CefRoot) {
  $cefPointer = Join-Path $launcherRoot '.tools/cef-root.txt'
  if (Test-Path -LiteralPath $cefPointer) { $CefRoot = (Get-Content -LiteralPath $cefPointer -Raw).Trim() }
}
if (-not $CefRoot -or -not (Test-Path -LiteralPath (Join-Path $CefRoot 'cmake/FindCEF.cmake'))) {
  throw 'CEF SDK missing. Run npm run cef:fetch, or pass -CefRoot with an extracted distribution.'
}
$vsRoot = 'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools'
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
  & $cmake -S $launcherRoot -B $buildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release -DXENON_NEXT_BUILD_DESKTOP=ON "-DCEF_ROOT=$CefRoot" "-DCMAKE_MAKE_PROGRAM=$ninja"
  if ($LASTEXITCODE -ne 0) { throw 'Desktop configure failed' }
  & $cmake --build $buildDirectory
  if ($LASTEXITCODE -ne 0) { throw 'Desktop build failed. Close any running launcher before rebuilding.' }
  if (-not $BuildOnly) { & (Join-Path $buildDirectory 'desktop/xenon_launcher_next.exe') }
} finally { Pop-Location }
