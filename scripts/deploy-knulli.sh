#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

usage() {
  cat >&2 <<'EOF'
Usage: scripts/deploy-knulli.sh [options] <ssh-host>

Options:
  --root <path>             Knulli userdata root. Default: /userdata
  --ports-dir <path>        Full ports directory. Default: <root>/roms/ports
  --autoinstall             Copy theswapper.zip to PortMaster autoinstall.
  --autoinstall-dir <path>  Autoinstall directory. Default: <root>/system/.local/share/PortMaster/autoinstall
  --gamefiles-dir <path>    Copy unmodified Steam game files into gamedata.
  --reset-setup             Re-run first-launch setup on next launch.

Environment:
  SWAPPER_DEPLOY_HOST
  SWAPPER_KNULLI_ROOT
  SWAPPER_KNULLI_PORTS_DIR
  SWAPPER_KNULLI_AUTOINSTALL_DIR
  SWAPPER_GAMEFILES_DIR
  SWAPPER_RESET_SETUP=1
  SWAPPER_AUTOINSTALL=1
EOF
}

host="${SWAPPER_DEPLOY_HOST:-}"
root="${SWAPPER_KNULLI_ROOT:-/userdata}"
ports_dir="${SWAPPER_KNULLI_PORTS_DIR:-}"
autoinstall_dir="${SWAPPER_KNULLI_AUTOINSTALL_DIR:-}"
gamefiles_dir="${SWAPPER_GAMEFILES_DIR:-}"
reset_setup="${SWAPPER_RESET_SETUP:-0}"
autoinstall="${SWAPPER_AUTOINSTALL:-0}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --root)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      root="$2"
      shift 2
      ;;
    --ports-dir)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      ports_dir="$2"
      shift 2
      ;;
    --autoinstall)
      autoinstall=1
      shift
      ;;
    --autoinstall-dir)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      autoinstall_dir="$2"
      shift 2
      ;;
    --gamefiles-dir)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      gamefiles_dir="$2"
      shift 2
      ;;
    --reset-setup)
      reset_setup=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage
      exit 2
      ;;
    *)
      if [ -n "$host" ]; then
        echo "Unexpected argument: $1" >&2
        usage
        exit 2
      fi
      host="$1"
      shift
      ;;
  esac
done

if [ -z "$host" ]; then
  usage
  exit 2
fi

if [ -z "$ports_dir" ]; then
  ports_dir="$root/roms/ports"
fi

if [ -z "$autoinstall_dir" ]; then
  autoinstall_dir="$root/system/.local/share/PortMaster/autoinstall"
fi

if [ "$autoinstall" = "1" ]; then
  make zip
  ssh "$host" 'sh -s' -- "$autoinstall_dir" <<'REMOTE_AUTOINSTALL_MKDIR'
set -e
mkdir -p "$1"
REMOTE_AUTOINSTALL_MKDIR
  scp "build/theswapper.zip" "$host:/tmp/theswapper.zip.tmp"
  ssh "$host" 'sh -s' -- "$autoinstall_dir" <<'REMOTE_AUTOINSTALL'
set -e
autoinstall_dir="$1"
mv /tmp/theswapper.zip.tmp "$autoinstall_dir/theswapper.zip"
ls -l "$autoinstall_dir/theswapper.zip"
REMOTE_AUTOINSTALL
  echo "Open PortMaster on the device to process the autoinstall zip."
  exit 0
fi

make package

package_root="build/package/ports/theswapper"
launcher="$package_root/The Swapper.sh"
payload="$package_root/theswapper"

for required in "$launcher" "$payload/libs.aarch64/libfmodex.so" "$payload/tools/setup" "$payload/tools/texture-downscale" "$payload/tools/xdg-open"; do
  if [ ! -e "$required" ]; then
    echo "Missing package file: $required" >&2
    exit 1
  fi
done

gamedir="$ports_dir/theswapper"

printf 'Deploying The Swapper to %s:%s\n' "$host" "$gamedir"

ssh "$host" 'sh -s' -- "$ports_dir" "$gamedir" <<'REMOTE_MKDIR'
set -e
ports_dir="$1"
gamedir="$2"
mkdir -p "$ports_dir" "$gamedir"
REMOTE_MKDIR

rsync -rt --delete --no-owner --no-group --omit-dir-times \
  --exclude gamedata/ \
  --exclude savedata-home/ \
  --exclude savedata/ \
  --exclude asset-patches/ \
  --exclude .setup_complete \
  "$payload/" "$host:$gamedir/"
rsync -rt --no-owner --no-group --omit-dir-times \
  "$package_root/README.md" \
  "$package_root/cover.png" \
  "$package_root/gameinfo.xml" \
  "$package_root/port.json" \
  "$package_root/screenshot.png" \
  "$host:$gamedir/"
scp "$launcher" "$host:$ports_dir/The Swapper.sh.tmp"

if [ -n "$gamefiles_dir" ]; then
  rsync -rt --no-owner --no-group --omit-dir-times "$gamefiles_dir/" "$host:$gamedir/gamedata/"
fi

ssh "$host" 'sh -s' -- "$ports_dir" "$gamedir" "$reset_setup" <<'REMOTE'
set -e
ports_dir="$1"
gamedir="$2"
reset_setup="$3"

pids="$(ps w | awk '/[m]ono TheSwapper\.exe/ { print $1 }')"
[ -n "$pids" ] && kill -TERM $pids 2>/dev/null || true
sleep 0.5
pids="$(ps w | awk '/[m]ono TheSwapper\.exe/ { print $1 }')"
[ -n "$pids" ] && kill -KILL $pids 2>/dev/null || true

mv "$ports_dir/The Swapper.sh.tmp" "$ports_dir/The Swapper.sh"
chmod 755 "$ports_dir/The Swapper.sh" "$gamedir/tools/setup" "$gamedir/tools/texture-downscale" "$gamedir/tools/xdg-open"

if [ "$reset_setup" = "1" ]; then
  rm -f \
    "$gamedir/.setup_complete" \
    "$gamedir/setup.log" \
    "$gamedir/log.txt"
  rm -rf "$gamedir/asset-patches"
fi

ls -l "$ports_dir/The Swapper.sh" "$gamedir/tools/setup" "$gamedir/tools/texture-downscale" "$gamedir/tools/xdg-open" "$gamedir/libs.aarch64/libfmodex.so"

if [ ! -f "$gamedir/gamedata/TheSwapper.exe" ]; then
  echo "Note: copy the Steam Windows files into $gamedir/gamedata before launching." >&2
fi
REMOTE
