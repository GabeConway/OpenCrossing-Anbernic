#!/bin/bash
# release.sh - Package and send a build to Discord
#
# Usage:
#   ./release.sh [--arm64|--amd64|--armhf] [--tag v1.2.3] [--webhook URL]
#
# Discord webhook URL can also be set via environment variable:
#   DISCORD_WEBHOOK=https://discord.com/api/webhooks/... ./release.sh
#
# Prerequisites: zip, curl

set -e

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
ARCH="arm64"
TAG=""
WEBHOOK="$(printf '%s' "${DISCORD_WEBHOOK:-}" | tr -d '\r\n\t ')"

while [ $# -gt 0 ]; do
    case "$1" in
        --arm64)  ARCH="arm64"  ;;
        --amd64)  ARCH="amd64"  ;;
        --armhf)  ARCH="armhf"  ;;
        --tag)    shift; TAG="$1" ;;
        --webhook) shift; WEBHOOK="$(printf '%s' "$1" | tr -d '\r\n\t ')" ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

case "$ARCH" in
    arm64) BUILD_BIN="$SCRIPT_DIR/pc/build64/bin" ;  BIN_NAME="AnimalCrossing.arm64" ;;
    amd64) BUILD_BIN="$SCRIPT_DIR/pc/buildamd64/bin" ; BIN_NAME="AnimalCrossing" ;;
    armhf) BUILD_BIN="$SCRIPT_DIR/pc/build32/bin" ;  BIN_NAME="AnimalCrossing" ;;
esac

LAUNCHER="$SCRIPT_DIR/port_files/Animal Crossing.sh"

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if [ ! -f "$BUILD_BIN/AnimalCrossing" ]; then
    echo "Error: binary not found at $BUILD_BIN/AnimalCrossing"
    echo "Run the build script first: ./build_ninja.sh --$ARCH"
    exit 1
fi

if [ ! -f "$LAUNCHER" ]; then
    echo "Error: launcher not found at $LAUNCHER"
    exit 1
fi

command -v zip  >/dev/null 2>&1 || { echo "Error: 'zip' is required. sudo apt install zip"; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "Error: 'curl' is required. sudo apt install curl"; exit 1; }

if [ -z "$WEBHOOK" ]; then
    echo "Error: Discord webhook URL required."
    echo "  Pass via --webhook URL or set DISCORD_WEBHOOK env var."
    exit 1
fi

# ---------------------------------------------------------------------------
# Determine version tag
# ---------------------------------------------------------------------------
if [ -z "$TAG" ]; then
    TAG="$(git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null || echo "dev")"
fi

RELEASE_NAME="AnimalCrossing-${TAG}-${ARCH}"
ZIP_FILE="/tmp/${RELEASE_NAME}.zip"

echo "=== Packaging $RELEASE_NAME ==="

# ---------------------------------------------------------------------------
# Build zip in a temp staging directory
# ---------------------------------------------------------------------------
STAGING="$(mktemp -d)"

# Root launcher: "Animal Crossing.sh"
cp "$LAUNCHER" "$STAGING/Animal Crossing.sh"
chmod +x "$STAGING/Animal Crossing.sh"

# /ac-gc/ — all files from the build bin dir
mkdir -p "$STAGING/ac-gc"
cp -r "$BUILD_BIN"/. "$STAGING/ac-gc/"

# Strip the binary in staging (never touches the original build output)
STAGED_BIN="$STAGING/ac-gc/AnimalCrossing"
if file "$STAGED_BIN" | grep -q "not stripped"; then
    echo "Stripping binary..."
    aarch64-linux-gnu-strip "$STAGED_BIN"
    echo "Stripped: $(du -sh "$STAGED_BIN" | cut -f1)"
else
    echo "Binary already stripped, skipping."
fi

# For arm64 builds the launcher expects AnimalCrossing.arm64
if [ "$ARCH" = "arm64" ] && [ -f "$STAGING/ac-gc/AnimalCrossing" ]; then
    chmod +x "$STAGING/ac-gc/AnimalCrossing"
fi

# Ensure save subdirs match the new card layout
mkdir -p "$STAGING/ac-gc/save/card_a"
mkdir -p "$STAGING/ac-gc/save/card_b"

# Keep rom/texture_pack as empty placeholder dirs (user fills these in)
mkdir -p "$STAGING/ac-gc/rom"
mkdir -p "$STAGING/ac-gc/texture_pack"

# ---------------------------------------------------------------------------
# Create zip (paths relative to staging root)
# ---------------------------------------------------------------------------
rm -f "$ZIP_FILE"
(cd "$STAGING" && zip -r "$ZIP_FILE" .)
echo "Created: $ZIP_FILE ($(du -sh "$ZIP_FILE" | cut -f1))"

# ---------------------------------------------------------------------------
# Send to Discord
# ---------------------------------------------------------------------------
COMMIT_MSG="$(git -C "$SCRIPT_DIR" log -1 --pretty=format:'%s' 2>/dev/null || echo "")"

# Build JSON payload in a temp file to avoid shell escaping issues with commit messages
PAYLOAD_FILE="$(mktemp)"
trap 'rm -rf "$STAGING" "$PAYLOAD_FILE"' EXIT
if [ -n "$COMMIT_MSG" ]; then
    printf '{"content":"**%s**\\n%s"}' "$RELEASE_NAME" "$COMMIT_MSG" > "$PAYLOAD_FILE"
else
    printf '{"content":"**%s**"}' "$RELEASE_NAME" > "$PAYLOAD_FILE"
fi

echo "=== Sending to Discord ==="
HTTP_STATUS=$(curl -w "%{http_code}" -o /dev/null \
    -F "payload_json=<${PAYLOAD_FILE};type=application/json" \
    -F "file=@${ZIP_FILE};filename=${RELEASE_NAME}.zip" \
    "$WEBHOOK") || true

if [ "$HTTP_STATUS" = "200" ] || [ "$HTTP_STATUS" = "204" ]; then
    echo "Sent! (HTTP $HTTP_STATUS)"
else
    echo "Discord upload failed (HTTP $HTTP_STATUS)"
    exit 1
fi

echo ""
echo "=== Done: $RELEASE_NAME ==="
