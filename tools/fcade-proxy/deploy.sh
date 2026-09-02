#!/usr/bin/env bash
set -euo pipefail
TARGET="${1:?Usage: ./deploy.sh user@host:/opt/fcade-proxy}"
DIR="$(cd "$(dirname "$0")" && pwd)"
rsync -avz --delete \
  --exclude 'fcade-cookie.txt' \
  "$DIR/fcade-proxy.js" \
  "$DIR/fcade-proxy.service" \
  "$DIR/package.json" \
  "$DIR/README.md" \
  "$DIR/../fcade-replays/fcade_replay_tool.py" \
  "$TARGET/"
echo "Now on the remote, run:"
echo "  sudo cp $TARGET/fcade-proxy.service /etc/systemd/system/"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl restart fcade-proxy"
echo ""
echo "NOTE: --exclude 'fcade-cookie.txt' keeps this rsync from ever wiping a"
echo "cookie already on the remote. The cookie is refreshed independently"
echo "(see README.md 'Cookie refresh' section) — it never lives in this repo"
echo "and this script never touches it."
