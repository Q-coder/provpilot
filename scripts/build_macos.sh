#!/bin/bash
# ProvPilot macOS Build & Assembly Script
# Builds, signs, and packages the ProvPilot.app into an app-only DMG.
# Scenery is shipped separately (not bundled — it's ~200 GB).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
GODOT="$REPO_ROOT/godot/bin/godot.macos.editor.double.arm64"
# The signing identity by SHA-1 hash, not by name: the keychain holds several
# "Developer ID Application: Gerhard Gubler (DUWB54DV3A)" certificates (the
# renewals keep the name) and codesign takes the first match by name, which
# was the one expiring Feb 2027 while the new one runs to Sep 2031.
# List them with: security find-identity -v -p codesigning
IDENTITY="AAA957C87686181A21896CAC7B99B2ECFC559E7E"   # Developer ID Application, valid to 2031-09-17
# Notarization: a notarytool keychain profile (xcrun notarytool
# store-credentials <name> --apple-id ... --team-id DUWB54DV3A); the DMG is
# submitted and stapled in step 7 when the profile exists.
NOTARY_PROFILE="${NOTARY_PROFILE:-provpilot-notary}"

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
echo "[1/7] Building GDExtensions (template_release, double, arm64)..."
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

# 1c. libworld3d (global globe) — separate repo (github.com/Q-coder/world3d),
# expected as a sibling checkout of this repo. The release dylib is a gitignored
# build artifact (see docs/WORLD3D.md); without it the export's shared-object
# copy loop fails and — because that loop breaks on the first error — can
# silently drop libscenery3d from Contents/Frameworks as well.
WORLD_SRC_DIR="$REPO_ROOT/../world3d"
WORLD_BUILT="$WORLD_SRC_DIR/demo/bin/libworld3d.macos.template_release.double.arm64.dylib"
WORLD_DYLIB="provpilot/bin/libworld3d.macos.template_release.double.arm64.dylib"
if [[ -d "$WORLD_SRC_DIR/src" ]]; then
    echo "  - libworld3d (sibling repo) ..."
    ( cd "$WORLD_SRC_DIR" && scons platform=macos arch=arm64 precision=double target=template_release \
        -j$(sysctl -n hw.ncpu) 2>&1 | tail -3 )
    if [[ ! -f "$WORLD_BUILT" ]]; then
        echo "ERROR: world3d release build produced no dylib at $WORLD_BUILT" >&2
        exit 1
    fi
    # Copy into provpilot/bin/ (where world3d.gdextension points) only if it
    # changed, so an unchanged source doesn't churn the signature/mtime.
    if ! cmp -s "$WORLD_BUILT" "$REPO_ROOT/$WORLD_DYLIB"; then
        echo "    updated → $WORLD_DYLIB"
        cp "$WORLD_BUILT" "$REPO_ROOT/$WORLD_DYLIB"
    else
        echo "    unchanged (already current)"
    fi
else
    echo "  - libworld3d source repo not found at $WORLD_SRC_DIR — using existing dylib in provpilot/bin/"
fi
echo ""

# Sanity-check all three extension dylibs are present.
PROV_DYLIB="provpilot/bin/libprovpilot.macos.template_release.double.arm64.dylib"
for f in "$PROV_DYLIB" "$SCEN_DYLIB" "$WORLD_DYLIB"; do
    if [[ ! -f "$REPO_ROOT/$f" ]]; then
        echo "ERROR: missing $f — build it first" >&2
        exit 1
    fi
done

# --- Step 2: Export Godot project ---
echo "[2/7] Exporting Godot project to $APP_PATH ..."
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
echo "[3/7] JSBSim bundling skipped — data is extracted on first run."
echo ""

# --- Step 4: Code-sign with Developer ID ---
echo "[4/7] Code-signing with Developer ID..."
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
echo "[5/7] Creating DMG at $DMG_PATH ..."
rm -f "$DMG_PATH"
# Stage just the .app so the DMG doesn't pick up build/ siblings.
STAGE_DIR=$(mktemp -d -t provpilot-dmg)
cp -R "$APP_PATH" "$STAGE_DIR/"
# the GPL text and the component list travel with the app
cp "$REPO_ROOT/LICENSE" "$STAGE_DIR/LICENSE.txt"
cp "$REPO_ROOT/NOTICE.md" "$STAGE_DIR/NOTICE.md"
hdiutil create -volname "ProvPilot ${VERSION}" \
    -srcfolder "$STAGE_DIR" -ov -format UDZO "$DMG_PATH" >/dev/null
rm -rf "$STAGE_DIR"
codesign --force --timestamp --sign "$IDENTITY" "$DMG_PATH"
echo ""

# --- Step 6: Summary + latest.json snippet ---
echo "[6/7] Build summary:"
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

# --- Step 7: Notarize + staple (skipped without a keychain profile) ---
NOTARY_CHECK="$(xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" 2>&1)" && NOTARY_OK=1 || NOTARY_OK=0
if [ "$NOTARY_OK" = 1 ]; then
    echo "[7/7] Notarizing with keychain profile '$NOTARY_PROFILE' (this uploads the DMG and waits)..."
    xcrun notarytool submit "$DMG_PATH" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG_PATH"
    spctl -a -t open --context context:primary-signature -v "$DMG_PATH"
elif ! echo "$NOTARY_CHECK" | grep -qi "keychain\|profile\|credential"; then
    # The profile exists but Apple refused (e.g. HTTP 403: a developer
    # agreement waits to be signed at developer.apple.com).
    echo "[7/7] NOT notarized: notarytool failed with:"
    echo "$NOTARY_CHECK" | sed 's/^/      /'
    echo "      after fixing: xcrun notarytool submit \"$DMG_PATH\" --keychain-profile $NOTARY_PROFILE --wait && xcrun stapler staple \"$DMG_PATH\""
    echo "      (stapling changes the DMG: recompute sha256 and size for latest.json)"
else
    echo "[7/7] No notarytool keychain profile '$NOTARY_PROFILE': not notarized. Create one with"
    echo "      xcrun notarytool store-credentials $NOTARY_PROFILE --apple-id <id> --team-id DUWB54DV3A"
    echo "      then: xcrun notarytool submit \"$DMG_PATH\" --keychain-profile $NOTARY_PROFILE --wait && xcrun stapler staple \"$DMG_PATH\""
fi
echo "Done."
