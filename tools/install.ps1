# Installs the engine fix into a game folder: build\dinput8.dll plus everything under mod\.
# An existing witcher_enginefix.ini is kept (pass -OverwriteIni to replace it).
# Usage: powershell -ExecutionPolicy Bypass -File tools\install.ps1 [-GamePath "D:\...\The Witcher Enhanced Edition"]
#        or set WITCHER_DIR once instead of passing -GamePath.
param(
    [string]$GamePath = $env:WITCHER_DIR,
    [switch]$OverwriteIni
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

if (-not $GamePath -or -not (Test-Path (Join-Path $GamePath 'System\witcher.exe'))) {
    throw "Game folder not found. Pass -GamePath or set WITCHER_DIR to the folder that contains System\witcher.exe."
}
if (Get-Process witcher -ErrorAction SilentlyContinue) {
    throw "witcher.exe is running. Close the game first (the DLL is locked while it runs)."
}
$dll = Join-Path $repo 'build\dinput8.dll'
if (-not (Test-Path $dll)) { throw "build\dinput8.dll not found. Run build.bat first." }

Copy-Item $dll (Join-Path $GamePath 'System\dinput8.dll') -Force
Write-Host "System\dinput8.dll"

$mod = Join-Path $repo 'mod'
Get-ChildItem $mod -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($mod.Length + 1)
    $dest = Join-Path $GamePath $rel
    if ($_.Name -eq 'witcher_enginefix.ini' -and (Test-Path $dest) -and -not $OverwriteIni) {
        Write-Host "$rel (kept existing)"
        return
    }
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Copy-Item $_.FullName $dest -Force
    Write-Host $rel
}

$loader = Join-Path $GamePath 'System\Scripts\testtools.lua'
if (-not (Test-Path $loader) -or -not (Select-String -Path $loader -Pattern '"enginefix_hook"' -Quiet)) {
    Write-Warning "System\Scripts\testtools.lua does not load enginefix_hook. The save-list Lua fix stays inactive until it does (see README)."
}
