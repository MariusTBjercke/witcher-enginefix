# Builds dist\WitcherEngineFix-<version>.zip, laid out to extract straight into the game folder
# (System\, README_WitcherEngineFix.txt, LICENSE_WitcherEngineFix.txt). Runs build.bat first unless
# -NoBuild. Entries use "/" separators so the zip also extracts correctly outside Windows
# (PowerShell 5.1's ZipFile writes "\").
param([string]$Version = "", [switch]$NoBuild)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
    $Version = (Select-String -Path (Join-Path $root "CHANGELOG.md") -Pattern '^## (\d+\.\d+\.\d+)' |
        Select-Object -First 1).Matches[0].Groups[1].Value
}
$src = Get-Content (Join-Path $root "src\enginefix.c") -Raw
if ($src -notmatch "#define ENGINEFIX_VERSION `"$([regex]::Escape($Version))`"") {
    throw "ENGINEFIX_VERSION in src\enginefix.c does not match $Version from CHANGELOG.md"
}

if (-not $NoBuild) {
    & cmd /c "`"$root\build.bat`""
    if ($LASTEXITCODE -ne 0) { throw "build.bat failed" }
}

# Published path in the zip -> source path in the repo.
$sources = [ordered]@{
    "README_WitcherEngineFix.txt"  = "docs\README.txt"
    "LICENSE_WitcherEngineFix.txt" = "LICENSE"
    "System/dinput8.dll"           = "build\dinput8.dll"
    "System"                       = "mod\System"
}

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zipPath = Join-Path $dist "WitcherEngineFix-$Version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $sources.GetEnumerator()) {
        $path = Join-Path $root $entry.Value
        $files = if (Test-Path $path -PathType Leaf) { @(Get-Item $path) } else { Get-ChildItem $path -Recurse -File }
        foreach ($file in $files) {
            $relative = if (Test-Path $path -PathType Leaf) { "" } else { $file.FullName.Substring($path.Length).TrimStart('\') }
            $name = if ($relative) { "$($entry.Key)/$($relative -replace '\\', '/')" } else { $entry.Key }
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $name)
        }
    }
}
finally { $zip.Dispose() }

Write-Output "Built $zipPath"
