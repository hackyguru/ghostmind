#!/usr/bin/env bash
# Install the freshly built Ghostmind core + UI into the local LogosBasecamp user dir.
#
# Build first:
#   (cd ghostmind-core && nix build 'path:.#lgx-portable' -o result-portable)
#   (cd ghostmind-ui   && nix build 'path:.#lgx-portable' -o result-portable \
#        --override-input ghostmind_core path:../ghostmind-core)
#
# Core dylibs get the rm + cp + `codesign --force --sign -` treatment: Apple
# Silicon validates every executable page at map time, and nix's ad-hoc
# signature (plus a reused inode from overwriting in place) makes the kernel
# SIGKILL Basecamp with "Code Signature Invalid" on the next launch.
#
# The wallet (keys, recovery words, conversations) lives in Basecamp's
# module_data for ghostmind_core and is NOT touched by a reinstall.
set -euo pipefail

BASE="$HOME/Library/Application Support/Logos/LogosBasecamp"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_LGX="$HERE/ghostmind-core/result-portable/logos-ghostmind_core-module-lib.lgx"
UI_LGX="$HERE/ghostmind-ui/result-portable/logos-ghostmind-module.lgx"
VARIANT="darwin-arm64"

for f in "$CORE_LGX" "$UI_LGX"; do
    [ -f "$f" ] || { echo "missing $f — build it first" >&2; exit 1; }
done
[ -d "$BASE/modules/delivery_module" ] || echo "WARNING: delivery_module is not installed in $BASE/modules — Ghostmind's messaging needs it (see README)" >&2

echo "==> Quitting Basecamp"
osascript -e 'tell application "LogosBasecamp" to quit' >/dev/null 2>&1 || true
for _ in $(seq 1 20); do pgrep -f LogosBasecamp.bin >/dev/null || break; sleep 0.5; done
pkill -9 -f LogosBasecamp.bin >/dev/null 2>&1 || true
pkill -9 -f logos_host      >/dev/null 2>&1 || true
pkill -9 -f ui-host         >/dev/null 2>&1 || true
sleep 1

# The app used to be called Haven. Remove the old app code so the launcher
# doesn't show both. Its data (module_data/haven_core) stays: Ghostmind copies
# the wallet and chats from it on first start.
for old in "$BASE/plugins/haven" "$BASE/modules/haven_core"; do
    [ -d "$old" ] && { echo "==> Removing old Haven install: $old"; rm -rf "$old"; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/core" "$TMP/ui"
tar -xzf "$CORE_LGX" -C "$TMP/core"
tar -xzf "$UI_LGX"   -C "$TMP/ui"

echo "==> Installing core -> modules/ghostmind_core"
DEST="$BASE/modules/ghostmind_core"
mkdir -p "$DEST"
for f in "$TMP/core/variants/$VARIANT"/*; do
    name="$(basename "$f")"
    rm -f "$DEST/$name"                 # fresh inode — see header comment
    cp "$f" "$DEST/$name"
    chmod u+w "$DEST/$name"
    case "$name" in
        *.dylib)
            codesign --force --sign - "$DEST/$name" >/dev/null 2>&1
            xattr -c "$DEST/$name" 2>/dev/null || true
            codesign -v "$DEST/$name" || { echo "codesign FAILED for $name" >&2; exit 1; }
            ;;
    esac
    echo "    $name"
done
cp "$TMP/core/manifest.json" "$DEST/manifest.json"
printf '%s' "$VARIANT" > "$DEST/variant"

echo "==> Installing UI -> plugins/ghostmind"
DEST="$BASE/plugins/ghostmind"
mkdir -p "$DEST"
cp -R "$TMP/ui/variants/$VARIANT/." "$DEST/"
# The manifest points `icon` at assets/…, which lives at the package root
# rather than inside the variant — without this the sidebar shows a text tile.
[ -d "$TMP/ui/assets" ] && cp -R "$TMP/ui/assets" "$DEST/assets"
cp "$TMP/ui/manifest.json" "$DEST/manifest.json"
printf '%s' "$VARIANT" > "$DEST/variant"
chmod -R u+w "$DEST"
ls "$DEST" | sed 's/^/    /'

echo "==> Done. Launch with:  open /Applications/LogosBasecamp.app"
