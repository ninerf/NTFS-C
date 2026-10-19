#!/bin/bash
# Installs ntfs (read/write NTFS drives) plus macFUSE and ntfs-3g.
set -euo pipefail
REPO="ninerf/NTFS-C"

[ "$(uname -s)" = Darwin ] || { echo "ntfs only runs on macOS."; exit 1; }

BREW=$(command -v brew || echo /opt/homebrew/bin/brew)
[ -x "$BREW" ] || { echo "Install Homebrew first: https://brew.sh"; exit 1; }

echo "==> Installing macFUSE and ntfs-3g (asks for your password)"
"$BREW" install --cask macfuse
"$BREW" install gromgit/fuse/ntfs-3g-mac

echo "==> Installing ntfs to /usr/local/bin"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if ! curl -fsSL "https://github.com/$REPO/releases/latest/download/ntfs" -o "$tmp/ntfs"; then
    echo "    No prebuilt binary found, building from source"
    curl -fsSL "https://raw.githubusercontent.com/$REPO/main/ntfs.c" -o "$tmp/ntfs.c"
    cc -O2 -o "$tmp/ntfs" "$tmp/ntfs.c" -framework CoreFoundation -framework DiskArbitration
fi
sudo install -d /usr/local/bin
sudo install -m 755 "$tmp/ntfs" /usr/local/bin/ntfs

cat <<'EOF'

Done! One last step: allow macFUSE (only needed once).
  1. Shut down. Hold the power button until "Options" appears, open it.
  2. Utilities > Startup Security Utility > Reduced Security >
     tick "Allow user management of kernel extensions". Restart.
  3. Run `ntfs mount`, click Allow in System Settings > Privacy & Security,
     and restart again.

Then plug in a drive and run:  ntfs mount
EOF
