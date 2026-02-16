#!/usr/bin/env pwsh
# setup_windows.ps1 — One-time Windows setup for ProvPilot
# Run from the repo root: .\scripts\setup_windows.ps1

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
$assetsDir = Join-Path $repoRoot "provpilot\Assets"
$sceneryDir = Join-Path $repoRoot "scenery"

Write-Host "ProvPilot Windows Setup" -ForegroundColor Cyan
Write-Host "Repo root: $repoRoot"

# --- Create directory junctions for scenery assets ---
# On macOS these are symlinks; on Windows we use junctions (no admin rights needed)

$junctions = @(
    @{ Link = Join-Path $assetsDir "buildings"; Target = Join-Path $sceneryDir "buildings" },
    @{ Link = Join-Path $assetsDir "roads";     Target = Join-Path $sceneryDir "roads" },
    @{ Link = Join-Path $assetsDir "water";     Target = Join-Path $sceneryDir "water" }
)

foreach ($j in $junctions) {
    $link = $j.Link
    $target = $j.Target

    if (Test-Path $link) {
        $item = Get-Item $link -Force
        if ($item.LinkType -eq "Junction") {
            Write-Host "  [OK] Junction already exists: $link" -ForegroundColor Green
            continue
        }
        # Remove broken symlink text file from Git
        Write-Host "  Removing broken symlink file: $link" -ForegroundColor Yellow
        Remove-Item $link -Force
    }

    if (-not (Test-Path $target)) {
        Write-Host "  [SKIP] Target not found: $target" -ForegroundColor Yellow
        continue
    }

    cmd /c mklink /J "$link" "$target" | Out-Null
    Write-Host "  [CREATED] Junction: $link -> $target" -ForegroundColor Green
}

Write-Host ""
Write-Host "Done. Open the Godot project — first launch will reimport GLB files." -ForegroundColor Cyan
Write-Host "Build the GDExtension DLL with: scons arch=x86_64" -ForegroundColor Cyan
