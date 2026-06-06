#!/bin/bash
# ProvPilot macOS Build & Assembly Script
# Builds, signs, and packages the ProvPilot.app into an app-only DMG.
# Scenery is shipped separately (not bundled — it's ~200 GB).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
GODOT="$REPO_ROOT/godot/bin/godot.macos.editor.double.arm64"
IDENTITY="Developer ID Application: Gerhard Gubler (DUWB54DV3A)"

# Derive version from project.godot so the DMG filename matches what the app
# reports and what `latest.json` advertises.
VERSION=$(grep '^config/version=' "$REPO_ROOT/provpilot/project.godot" \
    | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
if [[ -z "$VERSION" ]]; then
    echo "ERROR: could not read config/version from project.godot" >&2
    exit 1
fi
APP_PATH="$BUILD_DIR/ProvPilot.app"
DMG_PATH="$BUILD_DIR/ProvPilot-${VERSION}-arm64.dmg"

echo "=== ProvPilot macOS Build — v${VERSION} ==="
echo "Repo root: $REPO_ROOT"
echo ""

# --- Step 1: Build BOTH GDExtensions (double precision, release) ---
echo "[1/6] Building GDExtensions (template_release, double, arm64)..."
cd "$REPO_ROOT"
if [[ -d ".venv" ]]; then
    source .venv/bin/activate
fi

# 1a. libprovpilot (JSBSim bridge) — builds into provpilot/bin/ directly.
echo "  - libprovpilot ..."
scons platform=macos arch=arm64 precision=double target=template_release \
    -j$(sysctl -n hw.ncpu) 2>&1 | tail -3

# 1b. libscenery3d — built from its own submodule. build_macos.sh USED to
# assume this dylib was already up-to-date and only re-signed it, which
# silently shipped a stale dylib (e.g. missing the runtime manager-creation
# fix → no buildings/roads/water in the release app while the editor was
# fine). Build it here so the release always matches the source.
SCEN_SRC_DIR="$REPO_ROOT/provpilot/addons/scenery3d"
SCEN_BUILT="$SCEN_SRC_DIR/demo/bin/libscenery3d.macos.template_release.double.arm64.dylib"
SCEN_DYLIB="provpilot/bin/libscenery3d.macos.template_release.double.arm64.dylib"
if [[ -d "$SCEN_SRC_DIR/src" ]]; then
    echo "  - libscenery3d (submodule) ..."
    ( cd "$SCEN_SRC_DIR" && scons precision=double arch=arm64 target=template_release \
        -j$(sysctl -n hw.ncpu) 2>&1 | tail -3 )
    if [[ ! -f "$SCEN_BUILT" ]]; then
        echo "ERROR: scenery3d release build produced no dylib at $SCEN_BUILT" >&2
        exit 1
    fi
    # Copy into provpilot/bin/ (where scenery3d.gdextension points) only if it
    # changed, so an unchanged source doesn't churn the signature/mtime.
    if ! cmp -s "$SCEN_BUILT" "$REPO_ROOT/$SCEN_DYLIB"; then
        echo "    updated → $SCEN_DYLIB"
        cp "$SCEN_BUILT" "$REPO_ROOT/$SCEN_DYLIB"
    else
        echo "    unchanged (already current)"
    fi
else
    echo "  - libscenery3d submodule not checked out — using existing dylib in provpilot/bin/"
fi
echo ""

# Sanity-check both extension dylibs are present.
PROV_DYLIB="provpilot/bin/libprovpilot.macos.template_release.double.arm64.dylib"
for f in "$PROV_DYLIB" "$SCEN_DYLIB"; do
    if [[ ! -f "$REPO_ROOT/$f" ]]; then
        echo "ERROR: missing $f — build it first" >&2
        exit 1
    fi
done

# --- Step 2: Export Godot project ---
echo "[2/6] Exporting Godot project to $APP_PATH ..."
mkdir -p "$BUILD_DIR"
rm -rf "$APP_PATH"
"$GODOT" --headless --path "$REPO_ROOT/provpilot" \
    --export-release "macOS" "$APP_PATH" 2>&1 | grep -E "ERROR|WARNING|DONE" || true
if [[ ! -d "$APP_PATH" ]]; then
    echo "ERROR: export failed — $APP_PATH not produced" >&2
    exit 1
fi
echo ""

# --- Step 3: (deprecated) JSBSim bundling no longer needed ---
# JSBSim aircraft data is now loaded from `<install_root>/aircraft/<id>/jsbsim/`,
# which the AircraftInstaller autoload extracts from the PCK on first run.
# The XMLs are packed into the PCK via the `include_filter` in export_presets.cfg.
echo "[3/6] JSBSim bundling skipped — data is extracted on first run."
echo ""

# --- Step 4: Code-sign with Developer ID ---
echo "[4/6] Code-signing with Developer ID..."
ENTITLEMENTS=$(mktemp)
cat > "$ENTITLEMENTS" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.disable-library-validation</key>
    <true/>
</dict>
</plist>
EOF

# Sign inside-out: every embedded dylib first, then the bundle.
for dylib in "$APP_PATH/Contents/Frameworks/"*.dylib; do
    [[ -f "$dylib" ]] || continue
    echo "  Signing $(basename "$dylib")"
    codesign --force --timestamp --options runtime \
        --sign "$IDENTITY" "$dylib"
done
codesign --force --timestamp --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$APP_PATH"
rm "$ENTITLEMENTS"
echo "  $(codesign -dvv "$APP_PATH" 2>&1 | grep Authority | head -1)"
echo ""

# --- Step 5: Build app-only DMG ---
echo "[5/6] Creating DMG at $DMG_PATH ..."
rm -f "$DMG_PATH"
# Stage just the .app so the DMG doesn't pick up build/ siblings.
STAGE_DIR=$(mktemp -d -t provpilot-dmg)
cp -R "$APP_PATH" "$STAGE_DIR/"
hdiutil create -volname "ProvPilot ${VERSION}" \
    -srcfolder "$STAGE_DIR" -ov -format UDZO "$DMG_PATH" >/dev/null
rm -rf "$STAGE_DIR"
echo ""

# --- Step 6: Summary + latest.json snippet ---
echo "[6/6] Build summary:"
APP_SIZE=$(du -sh "$APP_PATH" | awk '{print $1}')
DMG_SIZE=$(stat -f%z "$DMG_PATH")
DMG_SIZE_H=$(du -sh "$DMG_PATH" | awk '{print $1}')
DMG_SHA=$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')
echo "  App:  $APP_SIZE   $APP_PATH"
echo "  DMG:  $DMG_SIZE_H   $DMG_PATH"
echo "  SHA256: $DMG_SHA"
echo "  Bytes:  $DMG_SIZE"
echo ""
echo "latest.json macos_arm64 block:"
cat << EOF
    "macos_arm64": {
        "url": "$(basename "$DMG_PATH")",
        "sha256": "$DMG_SHA",
        "size": $DMG_SIZE
    }
EOF
echo ""
echo "Done."
