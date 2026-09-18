#!/usr/bin/env pwsh
# build_windows.ps1 - ProvPilot Windows Build Script
#
# Builds all components in the correct order:
#   1. JSBSim    (static, /MT CRT, Release)
#   2. Godot     (editor + template_release, double precision)
#   3. godot-cpp (double precision, generates extension_api.json)
#   4. ProvPilot GDExtension DLL
#   5. Scenery3D GDExtension DLL (submodule provpilot\addons\scenery3d)
#   5b. world3d GDExtension DLL (sibling checkout ..\world3d — clone
#       git@gitlab.md80.ch:gery/world3d.git next to this repo first)
#
# Prerequisites (must be on PATH):
#   - Visual Studio 2019+ (run from a VS Developer Command Prompt, or let this
#     script locate vcvars64.bat automatically)
#   - CMake, SCons  (pip install scons)
#   - Python 3
#
# Usage (from repo root, or anywhere):
#   .\scripts\build_windows.ps1 [options]
#
# Options:
#   -SkipJSBSim      Skip JSBSim rebuild (use if already built)
#   -SkipGodot       Skip Godot engine rebuild
#   -SkipGodotCpp    Skip godot-cpp rebuild
#   -Target          'editor' or 'template_release' or 'both' (default: both)
#   -Jobs            Parallel build jobs (default: logical CPU count)

param(
    [switch]$SkipJSBSim,
    [switch]$SkipGodot,
    [switch]$SkipGodotCpp,
    [switch]$SkipScenery3D,
    [switch]$SkipWorld3D,
    [ValidateSet('editor','template_release','both')]
    [string]$Target = 'both',
    [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path $PSScriptRoot -Parent
$jsbsimDir  = Join-Path $repoRoot 'jsbsim'
$godotDir   = Join-Path $repoRoot 'godot'
$godotCppDir  = Join-Path $repoRoot 'godot-cpp'
$scenery3dDir  = Join-Path $repoRoot 'provpilot\addons\scenery3d'
$world3dDir    = Join-Path (Split-Path $repoRoot -Parent) 'world3d'

# The double-precision editor binary name Godot produces
$godotExe   = Join-Path $godotDir 'bin\godot.windows.editor.double.x86_64.exe'

Write-Host ''
Write-Host '================================================' -ForegroundColor Cyan
Write-Host '  ProvPilot Windows Build  (double precision)  ' -ForegroundColor Cyan
Write-Host '================================================' -ForegroundColor Cyan
Write-Host "  Repo root : $repoRoot"
Write-Host "  Jobs      : $Jobs"
Write-Host "  Target    : $Target"
Write-Host ''

# ---------------------------------------------------------------------------
# Helper: locate and invoke vcvars64.bat if cl.exe is not already available
# ---------------------------------------------------------------------------
function Invoke-VcVars {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host '[vcvars] MSVC already on PATH, skipping vcvars64.' -ForegroundColor DarkGray
        return
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found. Install Visual Studio 2019 or newer with C++ Desktop workload.'
    }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw 'No Visual Studio installation with C++ tools found.'
    }

    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found at: $vcvars"
    }

    Write-Host "[vcvars] Loading MSVC environment from: $vcvars" -ForegroundColor DarkGray
    # Capture env vars set by vcvars and apply them to the current process
    $output = cmd /c "`"$vcvars`" && set" 2>&1
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    Write-Host '[vcvars] MSVC environment loaded.' -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Helper: ensure a submodule's working tree matches the commit the parent
# repo records (the gitlink). A plain `git pull` on the parent advances the
# gitlink but does NOT check out the submodule's files, so the source can be
# stale while the gitlink points at newer code. Building then silently ships
# an out-of-date binary. This syncs the working tree to the recorded commit.
# ---------------------------------------------------------------------------
function Update-Submodule {
    param([string]$RelPath)

    # git pathspecs and HEAD:<path> lookups require forward slashes.
    $gitPath = $RelPath -replace '\\', '/'
    $subPath = Join-Path $repoRoot $RelPath
    if (-not (Test-Path (Join-Path $subPath '.git'))) {
        Write-Host "  [submodule] $RelPath not initialized - running submodule update..." -ForegroundColor Yellow
        cmd /c "git -C `"$repoRoot`" submodule update --init --recursive -- `"$gitPath`" 2>&1"
        return
    }

    # Commit recorded by the parent index (source of truth for the build).
    $recorded = (& git -C $repoRoot rev-parse "HEAD:$gitPath" 2>$null)
    $current  = (& git -C $subPath  rev-parse HEAD 2>$null)
    if (-not $recorded -or -not $current) {
        Write-Host "  [submodule] $RelPath - could not determine commits, skipping sync check." -ForegroundColor Yellow
        return
    }

    if ($recorded -eq $current) {
        Write-Host "  [submodule] $RelPath up to date ($($current.Substring(0,7)))." -ForegroundColor DarkGray
        return
    }

    Write-Host "  [submodule] $RelPath out of sync:" -ForegroundColor Yellow
    Write-Host "      checked out : $($current.Substring(0,7))" -ForegroundColor Yellow
    Write-Host "      parent wants: $($recorded.Substring(0,7))" -ForegroundColor Yellow
    Write-Host "  [submodule] Syncing working tree to recorded commit..." -ForegroundColor Yellow
    cmd /c "git -C `"$repoRoot`" submodule update --init --recursive -- `"$gitPath`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to sync submodule $RelPath to $($recorded.Substring(0,7)). " +
              "Commit or stash local changes in $RelPath, then re-run."
    }
    $now = (& git -C $subPath rev-parse HEAD 2>$null)
    Write-Host "  [submodule] $RelPath now at $($now.Substring(0,7))." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Step 1 - Build JSBSim (static library, /MT CRT)
# ---------------------------------------------------------------------------
if (-not $SkipJSBSim) {
    Write-Host ''
    Write-Host '[1/5] Building JSBSim (static, Release, /MT CRT)...' -ForegroundColor Yellow

    Invoke-VcVars

    $buildDir = Join-Path $jsbsimDir 'build'
    if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory $buildDir | Out-Null }

    Push-Location $buildDir
    try {
        # Configure
        cmd /c "cmake -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_PYTHON_MODULE=OFF .. 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

        # Patch /MD -> /MT in all flags.make files so the CRT matches Godot
        Write-Host '  Patching CRT flags (/MD -> /MT)...' -ForegroundColor DarkGray
        Get-ChildItem -Recurse -Filter 'flags.make' | ForEach-Object {
            $content = Get-Content $_.FullName -Raw
            if ($content -match '-MD') {
                $content -replace '-MDd?', '-MT' | Set-Content $_.FullName -NoNewline
                Write-Host "    Patched: $($_.FullName)" -ForegroundColor DarkGray
            }
        }
        # Remove stale objects that were compiled with /MD
        Get-ChildItem -Recurse -Filter '*.obj' | Remove-Item -Force -ErrorAction SilentlyContinue

        # Build (run via cmd so stderr stays plain text and doesn't trip PowerShell's error handling)
        cmd /c "nmake 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "nmake failed (exit $LASTEXITCODE)" }

        $lib = Join-Path $buildDir 'src\JSBSim.lib'
        if (-not (Test-Path $lib)) { throw "Expected output not found: $lib" }
        Write-Host "  [OK] $lib" -ForegroundColor Green
    }
    finally { Pop-Location }
} else {
    Write-Host '[1/5] Skipping JSBSim (--SkipJSBSim set).' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Step 2 - Build Godot (double precision)
# ---------------------------------------------------------------------------
if (-not $SkipGodot) {
    Write-Host ''
    Write-Host '[2/5] Building Godot 4 (platform=windows, precision=double)...' -ForegroundColor Yellow

    Invoke-VcVars

    Push-Location $godotDir
    try {
        $targets = if ($Target -eq 'both') { @('editor','template_release') } else { @($Target) }

        foreach ($t in $targets) {
            Write-Host "  Building target=$t ..." -ForegroundColor DarkGray
            cmd /c "scons platform=windows arch=x86_64 target=$t precision=double d3d12=no metal=no production=yes -j$Jobs 2>&1"
            if ($LASTEXITCODE -ne 0) { throw "Godot scons $t failed (exit $LASTEXITCODE)" }

            $expectedBin = Join-Path $godotDir "bin\godot.windows.$t.double.x86_64.exe"
            if (-not (Test-Path $expectedBin)) {
                throw "Expected binary not found: $expectedBin"
            }
            Write-Host "  [OK] $expectedBin" -ForegroundColor Green
        }
    }
    finally { Pop-Location }
} else {
    Write-Host '[2/5] Skipping Godot build (--SkipGodot set).' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Step 2b - Install compiled templates so the Godot export dialog is happy
# ---------------------------------------------------------------------------
if (-not $SkipGodot) {
    Write-Host ''
    Write-Host '[2b] Installing export templates...' -ForegroundColor Yellow
    # Godot looks for templates at "major.minor.status.precision" - NOT the full custom build hash
    $godotVersionFull = (cmd /c "`"$godotExe`" --version 2>&1").Trim()
    # e.g. "4.6.stable.double.custom_build.87a1090e1" -> "4.6.stable.double"
    $godotVersionShort = ($godotVersionFull -split '\.custom_build')[0]
    $templatesDir = "$env:APPDATA\Godot\export_templates\$godotVersionShort"
    New-Item -ItemType Directory -Force $templatesDir | Out-Null
    $binDir = Join-Path $godotDir 'bin'
    Copy-Item "$binDir\godot.windows.template_release.double.x86_64.exe"         "$templatesDir\windows_release_x86_64.exe" -Force
    Copy-Item "$binDir\godot.windows.template_release.double.x86_64.console.exe" "$templatesDir\windows_debug_x86_64.exe"   -Force
    [System.IO.File]::WriteAllText("$templatesDir\version.txt", $godotVersionShort, [System.Text.UTF8Encoding]::new($false))
    Write-Host "  [OK] Templates -> $templatesDir" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Step 3 - Regenerate godot-cpp bindings (extension_api.json) and build
# ---------------------------------------------------------------------------
if (-not $SkipGodotCpp) {
    Write-Host ''
    Write-Host '[3/5] Building godot-cpp (precision=double)...' -ForegroundColor Yellow

    Invoke-VcVars

    # Dump extension API using the freshly compiled editor
    if (-not (Test-Path $godotExe)) {
        throw "Godot editor binary not found: $godotExe`nRun without -SkipGodot first."
    }

    $apiJson = Join-Path $godotCppDir 'gdextension\extension_api.json'
    Write-Host "  Dumping extension_api.json via: $godotExe" -ForegroundColor DarkGray
    Push-Location (Join-Path $godotCppDir 'gdextension')
    try {
        cmd /c "`"$godotExe`" --headless --dump-extension-api extension_api.json 2>&1"
        if (-not (Test-Path $apiJson)) { throw "extension_api.json was not created." }
        Write-Host "  [OK] $apiJson" -ForegroundColor Green
    }
    finally { Pop-Location }

    # Build godot-cpp
    Push-Location $godotCppDir
    try {
        cmd /c "scons platform=windows arch=x86_64 precision=double -j$Jobs 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "godot-cpp scons failed (exit $LASTEXITCODE)" }
        Write-Host '  [OK] godot-cpp built.' -ForegroundColor Green
    }
    finally { Pop-Location }
} else {
    Write-Host '[3/5] Skipping godot-cpp build (--SkipGodotCpp set).' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Step 4 - Build ProvPilot GDExtension DLL
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[4/5] Building ProvPilot GDExtension DLL (precision=double)...' -ForegroundColor Yellow

Invoke-VcVars

$extTargets = if ($Target -eq 'both') { @('template_debug','template_release') } else { @($Target) }
Push-Location $repoRoot
try {
    foreach ($t in $extTargets) {
        cmd /c "scons platform=windows arch=x86_64 target=$t precision=double -j$Jobs 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "ProvPilot scons target=$t failed (exit $LASTEXITCODE)" }
        $dll = Join-Path $repoRoot "provpilot\bin\libprovpilot.windows.$t.double.x86_64.dll"
        if (Test-Path $dll) {
            Write-Host "  [OK] $dll" -ForegroundColor Green
        } else {
            Write-Host "  [WARN] Could not locate $dll - check provpilot\bin" -ForegroundColor Yellow
        }
    }
}
finally { Pop-Location }

# ---------------------------------------------------------------------------
# Step 5 - Build Scenery3D GDExtension DLL
# ---------------------------------------------------------------------------
if (-not $SkipScenery3D) {
    Write-Host ''
    Write-Host '[5/5] Building Scenery3D GDExtension DLL (precision=double)...' -ForegroundColor Yellow

    Invoke-VcVars

    # Ensure the scenery3d working tree matches the commit the parent records.
    # Otherwise a `git pull` on the parent (which only advances the gitlink)
    # leaves stale source, scons reports "up to date", and we ship an old DLL.
    Update-Submodule 'provpilot\addons\scenery3d'

    # Scenery3D's SConstruct builds its own godot-cpp internally via SConscript,
    # so we just run one scons command from the scenery3d root.
    # Seed its godot-cpp with the double-precision extension_api.json first.
    $srcApi  = Join-Path $godotCppDir  'gdextension\extension_api.json'
    $destApi = Join-Path $scenery3dDir 'godot-cpp\gdextension\extension_api.json'
    if (Test-Path $srcApi) {
        Write-Host "  Copying double-precision extension_api.json to Scenery3D godot-cpp..." -ForegroundColor DarkGray
        Copy-Item $srcApi $destApi -Force
    } else {
        Write-Host "  [WARN] $srcApi not found - Scenery3D will use its bundled API file." -ForegroundColor Yellow
    }

    # Build Scenery3D (builds its godot-cpp and the extension in one pass)
    # Use a local target list so this step does not depend on $extTargets
    # surviving from Step 4 (defensive against any earlier scope/state issues).
    $s3dTargets = if ($Target -eq 'both') { @('template_debug','template_release') } else { @($Target) }
    Write-Host "  Scenery3D dir : $scenery3dDir" -ForegroundColor DarkGray
    Write-Host "  Targets       : $($s3dTargets -join ', ')" -ForegroundColor DarkGray
    Push-Location $scenery3dDir
    try {
        foreach ($t in $s3dTargets) {
            Write-Host "  --- scons target=$t ---" -ForegroundColor Cyan
            cmd /c "scons platform=windows arch=x86_64 target=$t precision=double -j$Jobs 2>&1"
            if ($LASTEXITCODE -ne 0) { throw "Scenery3D scons target=$t failed (exit $LASTEXITCODE)" }
            $dll = Join-Path $scenery3dDir "demo\bin\libscenery3d.windows.$t.double.x86_64.dll"
            if (Test-Path $dll) {
                $ts = (Get-Item $dll).LastWriteTime
                Write-Host "  [OK] $dll (built $ts)" -ForegroundColor Green
            } else {
                Write-Host "  [WARN] expected DLL missing: $dll" -ForegroundColor Yellow
            }
        }
        Write-Host '  [OK] Scenery3D built.' -ForegroundColor Green
    }
    finally { Pop-Location }

    # Copy DLL(s) from scenery3d/demo/bin/ -> provpilot/bin/
    # Use Python to write the .gdextension file without BOM (PowerShell Set-Content
    # adds a UTF-8 BOM which breaks Godot's INI parser).
    $s3dBinDir    = Join-Path $scenery3dDir 'demo\bin'
    $provpilotBin = Join-Path $repoRoot 'provpilot\bin'
    if (Test-Path $s3dBinDir) {
        Get-ChildItem $s3dBinDir -Filter 'libscenery3d.windows*.dll' | ForEach-Object {
            Copy-Item $_.FullName $provpilotBin -Force
            Write-Host "  [OK] Copied $($_.Name) -> provpilot\bin\" -ForegroundColor Green
        }
        # Write scenery3d.gdextension without BOM using .NET (PowerShell Set-Content
        # adds a UTF-8 BOM which breaks Godot's INI parser).
        $gdextPath = Join-Path $provpilotBin 'scenery3d.gdextension'
        $gdextLines = @(
            '[configuration]',
            '',
            'entry_symbol = "scenery3d_library_init"',
            'compatibility_minimum = "4.3"',
            'reloadable = true',
            '',
            '[libraries]',
            '',
            'macos.debug = "res://bin/libscenery3d.macos.template_debug.double.arm64.dylib"',
            'macos.release = "res://bin/libscenery3d.macos.template_release.double.arm64.dylib"',
            'windows.debug.x86_64 = "res://bin/libscenery3d.windows.template_debug.double.x86_64.dll"',
            'windows.release.x86_64 = "res://bin/libscenery3d.windows.template_release.double.x86_64.dll"',
            'linux.debug.x86_64 = "res://bin/libscenery3d.linux.template_debug.double.x86_64.so"',
            'linux.release.x86_64 = "res://bin/libscenery3d.linux.template_release.double.x86_64.so"',
            'linux.debug.arm64 = "res://bin/libscenery3d.linux.template_debug.arm64.so"',
            'linux.release.arm64 = "res://bin/libscenery3d.linux.template_release.arm64.so"'
        )
        [System.IO.File]::WriteAllLines($gdextPath, $gdextLines, [System.Text.UTF8Encoding]::new($false))
        Write-Host "  [OK] scenery3d.gdextension written (no BOM, Windows double entries)." -ForegroundColor Green
    } else {
        Write-Host "  [WARN] $s3dBinDir not found - check Scenery3D build output." -ForegroundColor Yellow
    }
} else {
    Write-Host '[5/5] Skipping Scenery3D build (-SkipScenery3D set).' -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Step 5b - Build world3d (global globe) GDExtension DLL
# ---------------------------------------------------------------------------
# world3d is a separate repo (gitlab.md80.ch:gery/world3d), expected as a
# sibling checkout of this repo (..\world3d). The game cannot start without
# it since step C (the world is ECEF; W3DCoords/W3DGlobe are used everywhere).
if (-not $SkipWorld3D) {
    Write-Host ''
    Write-Host '[5b] Building world3d GDExtension DLL (precision=double)...' -ForegroundColor Yellow
    if (-not (Test-Path (Join-Path $world3dDir 'SConstruct'))) {
        throw "world3d checkout not found at $world3dDir - clone git@gitlab.md80.ch:gery/world3d.git next to this repo (branch globe-rebase-suppress) or pass -SkipWorld3D"
    }
    Invoke-VcVars
    # world3d's SConstruct defaults to its own double-precision API file when
    # precision=double, so no extension_api seeding is needed.
    $w3dTargets = if ($Target -eq 'both') { @('template_debug','template_release') } else { @($Target) }
    Write-Host "  world3d dir : $world3dDir" -ForegroundColor DarkGray
    Push-Location $world3dDir
    try {
        foreach ($t in $w3dTargets) {
            Write-Host "  --- scons target=$t ---" -ForegroundColor Cyan
            cmd /c "scons platform=windows arch=x86_64 target=$t precision=double -j$Jobs 2>&1"
            if ($LASTEXITCODE -ne 0) { throw "world3d scons target=$t failed (exit $LASTEXITCODE)" }
            $dll = Join-Path $world3dDir "demo\bin\libworld3d.windows.$t.double.x86_64.dll"
            if (Test-Path $dll) {
                Write-Host "  [OK] $dll" -ForegroundColor Green
            } else {
                Write-Host "  [WARN] Could not locate $dll" -ForegroundColor Yellow
            }
        }
        # Copy DLL(s) from world3d/demo/bin/ -> provpilot/bin/ (world3d.gdextension
        # there already lists the Windows entries).
        $provpilotBinW = Join-Path $repoRoot 'provpilot\bin'
        Get-ChildItem (Join-Path $world3dDir 'demo\bin') -Filter 'libworld3d.windows*.dll' | ForEach-Object {
            Copy-Item $_.FullName $provpilotBinW -Force
            Write-Host "  [OK] Copied $($_.Name) -> provpilot\bin\" -ForegroundColor Green
        }
    }
    finally { Pop-Location }
}

# ---------------------------------------------------------------------------
# Step 6 - Export project (headless)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[6/6] Exporting ProvPilot (Windows Desktop, release)...' -ForegroundColor Yellow

$buildOutput = Join-Path $repoRoot 'build'
$buildBinDir = Join-Path $buildOutput 'bin'
New-Item -ItemType Directory -Force $buildOutput | Out-Null
New-Item -ItemType Directory -Force $buildBinDir | Out-Null

cmd /c "`"$godotExe`" --path `"$(Join-Path $repoRoot 'provpilot')`" --headless --export-release `"Windows Desktop`" `"$(Join-Path $buildOutput 'ProvPilot.exe')`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "Godot export failed (exit $LASTEXITCODE)" }

# Copy GDExtension DLLs into build\bin\ (res://bin/ in the exported project)
$provpilotBin = Join-Path $repoRoot 'provpilot\bin'
Get-ChildItem $provpilotBin -Filter '*.windows.template_release.double.x86_64.dll' | ForEach-Object {
    Copy-Item $_.FullName $buildBinDir -Force
    Write-Host "  [OK] Copied $($_.Name) -> build\bin\" -ForegroundColor Green
}

Write-Host "  [OK] Export complete -> $buildOutput" -ForegroundColor Green

# JSBSim bundling is no longer needed at the top level. Aircraft data lives
# inside the PCK at res://aircraft/<id>/jsbsim/ and the AircraftInstaller
# autoload extracts it to <install_root>/aircraft/<id>/jsbsim/ on first run.
# jsbgodot.cpp resolves that path via the InstallRoot autoload.

# ---------------------------------------------------------------------------
# Step 7 - Package distributable ZIP
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[7/7] Creating distributable ZIP...' -ForegroundColor Yellow

# Read version from provpilot/project.godot  (config/version="x.y.z")
$projectGodot = Join-Path $repoRoot 'provpilot\project.godot'
$version = '1.0'
if (Test-Path $projectGodot) {
    $verLine = Select-String -Path $projectGodot -Pattern 'config/version\s*=' | Select-Object -First 1
    if ($verLine) {
        $version = ($verLine.Line -replace '.*=\s*"?([^"]+)"?.*', '$1').Trim()
    }
}

$zipName   = "ProvPilot-Windows-$version.zip"
$zipPath   = Join-Path $buildOutput $zipName
$stageDir  = Join-Path $buildOutput 'stage'

# Build a clean staging folder so the ZIP has a top-level ProvPilot\ folder
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
$stageRoot = Join-Path $stageDir 'ProvPilot'
New-Item -ItemType Directory -Force $stageRoot | Out-Null

# Copy distributable files into staging area
Copy-Item (Join-Path $buildOutput 'ProvPilot.exe')  $stageRoot -Force
Copy-Item (Join-Path $buildOutput 'ProvPilot.pck')  $stageRoot -Force
Copy-Item (Join-Path $repoRoot    'README.txt')      $stageRoot -Force
Copy-Item (Join-Path $buildOutput 'bin')    (Join-Path $stageRoot 'bin')    -Recurse -Force

# Create the ZIP (overwrite if it exists)
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath
Remove-Item $stageDir -Recurse -Force

$zipSizeMB = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host "  [OK] $zipName  ($zipSizeMB MB)" -ForegroundColor Green

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '================================================' -ForegroundColor Cyan
Write-Host '  Build complete!' -ForegroundColor Green
Write-Host '================================================' -ForegroundColor Cyan
Write-Host ''
Write-Host 'Distributable ZIP:' -ForegroundColor White
Write-Host "  build\$zipName" -ForegroundColor Green
Write-Host ''
Write-Host 'Contents (inside ProvPilot\ folder in the ZIP):' -ForegroundColor White
Write-Host '  ProvPilot.exe' -ForegroundColor DarkGray
Write-Host '  ProvPilot.pck' -ForegroundColor DarkGray
Write-Host '  README.txt' -ForegroundColor DarkGray
Write-Host '  bin\libprovpilot.windows.template_release.double.x86_64.dll' -ForegroundColor DarkGray
Write-Host '  bin\libscenery3d.windows.template_release.double.x86_64.dll' -ForegroundColor DarkGray
Write-Host '  bin\libworld3d.windows.template_release.double.x86_64.dll' -ForegroundColor DarkGray
Write-Host ''
Write-Host 'Users must keep this folder structure intact: the DLLs must stay in bin\' -ForegroundColor Yellow
Write-Host ''
