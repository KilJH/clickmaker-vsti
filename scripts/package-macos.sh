#!/bin/bash
# Turns a built ClickMaker.component into an installer (.pkg) and a zip, both written to dist/.
#
#   scripts/package-macos.sh <version> [path/to/ClickMaker.component]
#
# Optional environment:
#   SIGN_APPLICATION  codesign identity ("Developer ID Application: ..."); ad-hoc signature when empty
#   SIGN_INSTALLER    productbuild identity ("Developer ID Installer: ..."); unsigned installer when empty
#   NOTARY_KEY_PATH, NOTARY_KEY_ID, NOTARY_ISSUER
#                     App Store Connect API key; when set, the installer is notarized and stapled
set -euo pipefail

VERSION="${1:?usage: scripts/package-macos.sh <version> [path/to/ClickMaker.component]}"
COMPONENT="${2:-build/ClickMaker_artefacts/Release/AU/ClickMaker.component}"
PACKAGE_ID="com.clickmaker.clickmaker.au"
INSTALL_DIR="/Library/Audio/Plug-Ins/Components"
MIN_MACOS="14.0"
DIST="dist"
PKG="$DIST/ClickMaker-$VERSION-macOS.pkg"
ZIP="$DIST/ClickMaker-$VERSION-macOS-AU.zip"

if [[ ! -d "$COMPONENT" ]]; then
    echo "error: $COMPONENT not found; build the plugin first" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$DIST" "$WORK/payload"
PAYLOAD="$WORK/payload/ClickMaker.component"
ditto "$COMPONENT" "$PAYLOAD"

# Notarization requires the hardened runtime and a secure timestamp.
if [[ -n "${SIGN_APPLICATION:-}" ]]; then
    codesign --force --sign "$SIGN_APPLICATION" --options runtime --timestamp "$PAYLOAD"
else
    codesign --force --sign - "$PAYLOAD"
fi
codesign --verify --strict --verbose=2 "$PAYLOAD"

# A relocatable bundle would be installed wherever an older copy was moved to, not into Components.
pkgbuild --analyze --root "$WORK/payload" "$WORK/components.plist"
plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/components.plist"
pkgbuild --root "$WORK/payload" --component-plist "$WORK/components.plist" \
    --identifier "$PACKAGE_ID" --version "$VERSION" --install-location "$INSTALL_DIR" \
    "$WORK/ClickMakerAU.pkg"

# Listing arm64 keeps Apple Silicon Macs from asking to install Rosetta.
cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>ClickMaker $VERSION</title>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="$MIN_MACOS"/>
        </allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="$PACKAGE_ID"/>
    </choices-outline>
    <choice id="$PACKAGE_ID" title="ClickMaker AU">
        <pkg-ref id="$PACKAGE_ID"/>
    </choice>
    <pkg-ref id="$PACKAGE_ID" version="$VERSION" onConclusion="none">ClickMakerAU.pkg</pkg-ref>
</installer-gui-script>
EOF

rm -f "$PKG" "$ZIP"

if [[ -n "${SIGN_INSTALLER:-}" ]]; then
    productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" \
        --sign "$SIGN_INSTALLER" --timestamp "$PKG"
else
    productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$PKG"
fi

if [[ -n "${NOTARY_KEY_PATH:-}" ]]; then
    NOTARY_AUTH=(--key "$NOTARY_KEY_PATH" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER")
    xcrun notarytool submit "$PKG" "${NOTARY_AUTH[@]}" --wait --output-format json > "$WORK/notary.json"

    if [[ "$(plutil -extract status raw -o - "$WORK/notary.json")" != "Accepted" ]]; then
        xcrun notarytool log "$(plutil -extract id raw -o - "$WORK/notary.json")" "${NOTARY_AUTH[@]}" >&2
        exit 1
    fi

    xcrun stapler staple "$PKG"
fi

ditto -c -k --keepParent "$PAYLOAD" "$ZIP"
(cd "$DIST" && shasum -a 256 "$(basename "$PKG")" "$(basename "$ZIP")" > "ClickMaker-$VERSION-SHA256.txt")
ls -l "$DIST"
