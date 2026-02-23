#!/bin/bash
# ProvPilot macOS Build & Assembly Script
# Creates a distributable ProvPilot folder with the app and all data
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
DIST_DIR="$BUILD_DIR/ProvPilot"  # The distributable folder
GODOT="/Applications/Godot.app/Contents/MacOS/Godot"
IDENTITY="Developer ID Application: Gerhard Gubler (DUWB54DV3A)"

echo "=== ProvPilot macOS Build Script ==="
echo "Repo root: $REPO_ROOT"
echo ""

# --- Step 1: Build GDExtension ---
echo "[1/6] Building GDExtension (release, arm64)..."
cd "$REPO_ROOT"
if [[ -d ".venv" ]]; then
    source .venv/bin/activate
fi
scons target=template_release arch=arm64 -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
echo ""

# --- Step 2: Export Godot project ---
echo "[2/6] Exporting Godot project..."
mkdir -p "$BUILD_DIR"
rm -rf "$BUILD_DIR/ProvPilot.app"
"$GODOT" --headless --path "$REPO_ROOT/provpilot" \
    --export-release "macOS" "$BUILD_DIR/ProvPilot.app" 2>&1 | grep -E "ERROR|WARNING|DONE" || true
echo ""

# --- Step 3: Code-sign with Developer ID ---
echo "[3/6] Code-signing with Developer ID..."
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

# Sign inside-out
codesign --force --timestamp --options runtime \
    --sign "$IDENTITY" \
    "$BUILD_DIR/ProvPilot.app/Contents/Frameworks/libprovpilot.macos.template_release.arm64.dylib"
codesign --force --timestamp --options runtime \
    --sign "$IDENTITY" \
    "$BUILD_DIR/ProvPilot.app/Contents/Frameworks/libterrain.macos.release.framework"
codesign --force --timestamp --options runtime \
    --entitlements "$ENTITLEMENTS" \
    --sign "$IDENTITY" \
    "$BUILD_DIR/ProvPilot.app"
rm "$ENTITLEMENTS"
echo "Signature: $(codesign -dvv "$BUILD_DIR/ProvPilot.app" 2>&1 | grep Authority | head -1)"
echo ""

# --- Step 4: Assemble distribution folder ---
echo "[4/6] Assembling distribution folder..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# Move the .app into the distribution folder
mv "$BUILD_DIR/ProvPilot.app" "$DIST_DIR/"

# Copy JSBSim data (small — ~4MB)
echo "  Copying JSBSim data..."
mkdir -p "$DIST_DIR/jsbsim"
cp -R "$REPO_ROOT/jsbsim/aircraft" "$DIST_DIR/jsbsim/"
cp -R "$REPO_ROOT/jsbsim/engine" "$DIST_DIR/jsbsim/"
cp -R "$REPO_ROOT/jsbsim/systems" "$DIST_DIR/jsbsim/"

# Copy scenery data (large — ~184GB)
echo "  Copying scenery data (this will take a while)..."
cp -R "$REPO_ROOT/scenery" "$DIST_DIR/"

echo ""

# --- Step 5: Show summary ---
echo "[5/6] Build summary:"
echo "  App bundle:  $(du -sh "$DIST_DIR/ProvPilot.app" | awk '{print $1}')"
echo "  JSBSim data: $(du -sh "$DIST_DIR/jsbsim" | awk '{print $1}')"
echo "  Scenery:     $(du -sh "$DIST_DIR/scenery" | awk '{print $1}')"
echo "  Total:       $(du -sh "$DIST_DIR" | awk '{print $1}')"
echo ""

# --- Step 6: Optional DMG creation ---
echo "[6/6] Distribution folder ready at:"
echo "  $DIST_DIR"
echo ""
echo "To create a DMG (optional, for the full ~190GB):"
echo "  hdiutil create -volname ProvPilot -srcfolder '$DIST_DIR' -ov -format UDZO '$BUILD_DIR/ProvPilot.dmg'"
echo ""
echo "To test with symlinks instead of copying scenery:"
echo "  ln -s '$REPO_ROOT/scenery' '$DIST_DIR/scenery'"
echo "  ln -s '$REPO_ROOT/jsbsim' '$DIST_DIR/jsbsim'"
echo ""
echo "Done!"
