#!/usr/bin/env bash
set -euo pipefail
LOCAL="${1:?Usage: ./push-catalog.sh <local-catalog.json> user@host:/opt/fcade-proxy}"
TARGET="${2:?Usage: ./push-catalog.sh <local-catalog.json> user@host:/opt/fcade-proxy}"

if [ ! -f "$LOCAL" ]; then
  echo "error: $LOCAL does not exist" >&2
  exit 1
fi

rsync -avz "$LOCAL" "$TARGET/catalog.json"

echo ""
echo "Pushed $LOCAL -> $TARGET/catalog.json"
echo ""
echo "NOTE: no service restart needed. fcade-proxy.js re-reads"
echo "FCADE_CATALOG_FILE by mtime on every search/status request (see"
echo "README.md 'Offline catalog (option B)'), so this file takes effect"
echo "immediately -- the next search request after this rsync completes"
echo "will already be served from the new catalog."
echo ""
echo "If this is the first push to a fresh host, fix ownership/perms so the"
echo "fcade-proxy service user can read it, e.g.:"
echo "  ssh <host> 'sudo chown fcade-proxy:fcade-proxy /opt/fcade-proxy/catalog.json && sudo chmod 644 /opt/fcade-proxy/catalog.json'"
