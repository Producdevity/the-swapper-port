#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

host="${1:-${SWAPPER_DEPLOY_HOST:-}}"
if [ -z "$host" ]; then
  echo "Usage: $0 <ssh-host>" >&2
  echo "Or set SWAPPER_DEPLOY_HOST." >&2
  exit 2
fi

make package

package_root="build/package/ports/theswapper"
launcher="$package_root/The Swapper.sh"
payload="$package_root/theswapper"

for required in "$launcher" "$payload/libs.aarch64/libfmodex.so" "$payload/tools/setup.bash" "$payload/tools/xdg-open"; do
  if [ ! -e "$required" ]; then
    echo "Missing package file: $required" >&2
    exit 1
  fi
done

remote_root="$(ssh "$host" '
set -e
for root in /mnt/mmc /mnt/sdcard; do
  if [ -d "$root/ROMS/Ports" ] && [ -d "$root/ports" ]; then
    printf "%s\n" "$root"
    exit 0
  fi
done
echo "Could not find a writable muOS PortMaster root under /mnt/sdcard or /mnt/mmc." >&2
exit 1
')"

gamedir="$remote_root/ports/theswapper"
portdir="$remote_root/ROMS/Ports"
reset_setup="${SWAPPER_RESET_SETUP:-0}"

printf 'Deploying The Swapper to %s:%s\n' "$host" "$gamedir"
ssh "$host" 'sh -s' -- "$gamedir" "$portdir" <<'REMOTE_MKDIR'
set -e
gamedir="$1"
portdir="$2"
mkdir -p "$gamedir" "$portdir"
REMOTE_MKDIR

rsync -rt --delete --no-owner --no-group --omit-dir-times \
  --exclude gamedata/ \
  --exclude savedata-home/ \
  --exclude savedata/ \
  --exclude .setup_complete \
  "$payload/" "$host:$gamedir/"
scp "$launcher" "$host:$portdir/The Swapper.sh.tmp"

if [ -n "${SWAPPER_GAMEFILES_DIR:-}" ]; then
  rsync -rt --no-owner --no-group --omit-dir-times "$SWAPPER_GAMEFILES_DIR/" "$host:$gamedir/gamedata/"
fi

ssh "$host" 'sh -s' -- "$gamedir" "$portdir" "$reset_setup" <<'REMOTE'
set -e
gamedir="$1"
portdir="$2"
reset_setup="$3"

pids="$(ps w | awk '/[m]ono TheSwapper\.exe/ { print $1 }')"
[ -n "$pids" ] && kill -TERM $pids 2>/dev/null || true
sleep 0.5
pids="$(ps w | awk '/[m]ono TheSwapper\.exe/ { print $1 }')"
[ -n "$pids" ] && kill -KILL $pids 2>/dev/null || true

mv "$portdir/The Swapper.sh.tmp" "$portdir/The Swapper.sh"
chmod 755 "$portdir/The Swapper.sh" "$gamedir/tools/setup.bash" "$gamedir/tools/xdg-open"

if [ "$reset_setup" = "1" ]; then
  rm -f \
    "$gamedir/.setup_complete" \
    "$gamedir/setup.log" \
    "$gamedir/log.txt"
  rm -rf "$gamedir/asset-patches/normal-maps"
fi

ls -l "$portdir/The Swapper.sh" "$gamedir/tools/setup.bash" "$gamedir/tools/xdg-open" "$gamedir/libs.aarch64/libfmodex.so"

if [ ! -f "$gamedir/gamedata/TheSwapper.exe" ]; then
  echo "Note: copy the Steam Windows files into $gamedir/gamedata before launching." >&2
fi
REMOTE
