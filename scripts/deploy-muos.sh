#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

host="${SWAPPER_DEPLOY_HOST:-}"
autoinstall="${SWAPPER_AUTOINSTALL:-0}"
reset_profiles="${SWAPPER_RESET_PROFILES:-0}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --autoinstall)
      autoinstall=1
      shift
      ;;
    --reset-profiles)
      reset_profiles=1
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--autoinstall] [--reset-profiles] <ssh-host>" >&2
      echo "Or set SWAPPER_DEPLOY_HOST and optionally SWAPPER_AUTOINSTALL=1 or SWAPPER_RESET_PROFILES=1." >&2
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--autoinstall] [--reset-profiles] <ssh-host>" >&2
      exit 2
      ;;
    *)
      if [ -n "$host" ]; then
        echo "Unexpected argument: $1" >&2
        exit 2
      fi
      host="$1"
      shift
      ;;
  esac
done

if [ -z "$host" ]; then
  echo "Usage: $0 [--autoinstall] [--reset-profiles] <ssh-host>" >&2
  echo "Or set SWAPPER_DEPLOY_HOST and optionally SWAPPER_AUTOINSTALL=1 or SWAPPER_RESET_PROFILES=1." >&2
  exit 2
fi

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

if [ "$autoinstall" = "1" ]; then
  make zip
  autoinstall_dir="$remote_root/MUOS/PortMaster/autoinstall"
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

for required in \
  "$launcher" \
  "$payload/libs.aarch64/libfmodex.so" \
  "$payload/libs.aarch64/libtexture_astc.so" \
  "$payload/tools/setup" \
  "$payload/tools/mem-profile" \
  "$payload/tools/texture-astc-manifest" \
  "$payload/tools/texture-downscale" \
  "$payload/tools/xdg-open"; do
  if [ ! -e "$required" ]; then
    echo "Missing package file: $required" >&2
    exit 1
  fi
done

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
  --exclude asset-patches/ \
  --exclude .setup_complete \
  --exclude setup.log \
  --exclude log.txt \
  --exclude logs/ \
  "$payload/" "$host:$gamedir/"
rsync -rt --no-owner --no-group --omit-dir-times \
  "$package_root/README.md" \
  "$package_root/cover.png" \
  "$package_root/gameinfo.xml" \
  "$package_root/port.json" \
  "$package_root/screenshot.png" \
  "$host:$gamedir/"
scp "$launcher" "$host:$portdir/The Swapper.sh.tmp"

metadata_tmp="$(mktemp -d)"
trap 'rm -rf "$metadata_tmp"' EXIT
if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick is required for muOS catalogue artwork: brew install imagemagick" >&2
  exit 1
fi
python3 - "$package_root/gameinfo.xml" > "$metadata_tmp/The Swapper.txt" <<'PY'
import sys
import xml.etree.ElementTree as ET

tree = ET.parse(sys.argv[1])
desc = tree.find("./game/desc")
if desc is not None and desc.text:
    print(desc.text.strip().split("\n", 1)[0].strip())
PY
magick "$package_root/cover.png" -resize 320x240! PNG32:"$metadata_tmp/The Swapper-box.png"
magick "$package_root/screenshot.png" -resize 320x240! PNG24:"$metadata_tmp/The Swapper-preview.png"

ssh "$host" 'sh -s' -- "$remote_root" <<'REMOTE_CATALOG'
set -e
remote_root="$1"
catalog_dir="$remote_root/MUOS/info/catalogue/External - Ports"
mkdir -p "$catalog_dir/box" "$catalog_dir/preview" "$catalog_dir/text"
REMOTE_CATALOG
scp "$metadata_tmp/The Swapper-box.png" "$host:/tmp/theswapper-cover.png.tmp"
scp "$metadata_tmp/The Swapper-preview.png" "$host:/tmp/theswapper-screenshot.png.tmp"
scp "$metadata_tmp/The Swapper.txt" "$host:/tmp/theswapper-description.txt.tmp"

if [ -n "${SWAPPER_GAMEFILES_DIR:-}" ]; then
  rsync -rt --no-owner --no-group --omit-dir-times "$SWAPPER_GAMEFILES_DIR/" "$host:$gamedir/gamedata/"
fi

ssh "$host" 'sh -s' -- "$gamedir" "$portdir" "$reset_setup" "$reset_profiles" "$remote_root" <<'REMOTE'
set -e
gamedir="$1"
portdir="$2"
reset_setup="$3"
reset_profiles="$4"
remote_root="$5"
catalog_dir="$remote_root/MUOS/info/catalogue/External - Ports"

pids="$(ps w | awk '/[m]ono TheSwapper\.exe/ { print $1 }')"
[ -n "$pids" ] && kill -TERM $pids 2>/dev/null || true
sleep 0.5
pids="$(ps w | awk '/[m]ono TheSwapper\.exe/ { print $1 }')"
[ -n "$pids" ] && kill -KILL $pids 2>/dev/null || true

mv "$portdir/The Swapper.sh.tmp" "$portdir/The Swapper.sh"
mv /tmp/theswapper-cover.png.tmp "$catalog_dir/box/The Swapper.png"
mv /tmp/theswapper-screenshot.png.tmp "$catalog_dir/preview/The Swapper.png"
mv /tmp/theswapper-description.txt.tmp "$catalog_dir/text/The Swapper.txt"
chmod 755 "$portdir/The Swapper.sh" "$gamedir/tools/setup" "$gamedir/tools/mem-profile" "$gamedir/tools/texture-astc-manifest" "$gamedir/tools/texture-downscale" "$gamedir/tools/xdg-open"

if [ "$reset_setup" = "1" ]; then
  rm -f \
    "$gamedir/.setup_complete" \
    "$gamedir/setup.log" \
    "$gamedir/log.txt"
  rm -rf "$gamedir/asset-patches"
fi

if [ "$reset_profiles" = "1" ]; then
  profile_dir="$gamedir/savedata-home/.local/share/Facepalm Games/The Swapper 1000"
  mkdir -p "$profile_dir"
  cp -f "$gamedir/savedata-seed/"*.pro "$profile_dir/"
fi

ls -l "$portdir/The Swapper.sh" \
  "$catalog_dir/box/The Swapper.png" \
  "$catalog_dir/preview/The Swapper.png" \
  "$catalog_dir/text/The Swapper.txt" \
  "$gamedir/libs.aarch64/libtexture_astc.so" \
  "$gamedir/tools/setup" \
  "$gamedir/tools/mem-profile" \
  "$gamedir/tools/texture-astc-manifest" \
  "$gamedir/tools/texture-downscale" \
  "$gamedir/tools/xdg-open" \
  "$gamedir/libs.aarch64/libfmodex.so"

if [ ! -f "$gamedir/gamedata/TheSwapper.exe" ]; then
  echo "Note: copy the Steam Windows files into $gamedir/gamedata before launching." >&2
fi
REMOTE
