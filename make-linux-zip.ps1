# Makes survival-game-linux-source.zip to send to a friend on Linux.
# They unzip it and run:  bash play-linux.sh
# (the launcher installs what it needs, builds the game once, then starts it)
#
# Usage (PowerShell, in the game folder):  .\make-linux-zip.ps1  [-Out D:\survival-game-linux-source.zip]
param([string]$Out = (Join-Path $PSScriptRoot "survival-game-linux-source.zip"))
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = $PSScriptRoot
$Out = [System.IO.Path]::GetFullPath($Out)
# Stage next to the game folder, not inside it (editors lock files they are indexing).
$stage = Join-Path (Split-Path -Parent $root) "sg-linux-zip-staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path "$stage\survival-game" | Out-Null

try {
  # Only what's needed to build: no build folders, saves or editor files.
  foreach ($item in @("client", "server", "shared", "tests", "CMakeLists.txt", "README.md", "play-linux.sh", "package-linux.sh")) {
    Copy-Item (Join-Path $root $item) "$stage\survival-game\" -Recurse
  }
  # Shell scripts must have Linux line endings or bash refuses them.
  foreach ($sh in Get-ChildItem "$stage\survival-game\*.sh") {
    $text = [System.IO.File]::ReadAllText($sh.FullName) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($sh.FullName, $text, (New-Object System.Text.UTF8Encoding $false))
  }
  if (Test-Path $Out) { Remove-Item $Out -Force }
  # Write entries one by one with forward slashes: Linux unzip tools mangle backslash paths.
  $zip = [System.IO.Compression.ZipFile]::Open($Out, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($file in Get-ChildItem $stage -Recurse -File) {
      $name = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
      [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $name) | Out-Null
    }
  } finally { $zip.Dispose() }
} finally {
  Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Created $Out"
Write-Host "Send it to your friend. On Linux: unzip it, open the folder, run:  bash play-linux.sh"
