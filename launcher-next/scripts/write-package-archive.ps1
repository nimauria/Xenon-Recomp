param([Parameter(Mandatory=$true)][string]$SourceDirectory, [Parameter(Mandatory=$true)][string]$DestinationArchive)
$ErrorActionPreference = 'Stop'
$launcherZipSource = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd([char[]]@('\','/'))
$launcherZipTarget = [IO.Path]::GetFullPath($DestinationArchive)
if ($launcherZipTarget.StartsWith($launcherZipSource + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
  throw 'Write the archive outside its input directory'
}
$launcherZipEntries = @(Get-Item -LiteralPath $launcherZipSource) + @(Get-ChildItem -LiteralPath $launcherZipSource -Recurse -Force)
foreach ($launcherZipEntry in $launcherZipEntries) {
  if ($launcherZipEntry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package inputs must not contain links or junctions' }
}
if (Test-Path -LiteralPath $launcherZipTarget) { throw 'Choose a fresh archive filename' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$launcherZipArchive = [IO.Compression.ZipFile]::Open($launcherZipTarget, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($launcherZipEntry in $launcherZipEntries) {
    if ($launcherZipEntry.PSIsContainer) { continue }
    # .NET Framework's CreateFromDirectory can emit backslashes on Windows.
    # Explicit portable entry names keep the strict native extractor unchanged.
    $launcherZipName = $launcherZipEntry.FullName.Substring($launcherZipSource.Length + 1).Replace('\','/')
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($launcherZipArchive, $launcherZipEntry.FullName, $launcherZipName, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
  }
} finally { $launcherZipArchive.Dispose() }
