#!/bin/bash
set -e

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

# shellcheck source=/dev/null
source "$controlfolder/control.txt"

GAMEDIR="/$directory/ports/theswapper"
GAMEDATA="$GAMEDIR/gamedata"
SAVEDIR="$GAMEDIR/savedata-home"
PROFILE_DIR="$SAVEDIR/.local/share/Facepalm Games/The Swapper 1000"
SETUP_LOG="$GAMEDIR/setup.log"
NORMAL_MAP_MAX_DIMENSION=512
ATLAS_MAX_DIMENSION=1024
TEXTURE_STATE_DIR="$GAMEDIR/asset-patches/textures"
TEXTURE_MARKER="$TEXTURE_STATE_DIR/normal-${NORMAL_MAP_MAX_DIMENSION}-atlas-${ATLAS_MAX_DIMENSION}"
TEXTURE_DOWNSCALER="$GAMEDIR/tools/texture-downscale"
SETUP_MARKER="$GAMEDIR/.setup_complete"

mkdir -p "$GAMEDATA" "$PROFILE_DIR"
> "$SETUP_LOG"
exec > >(tee -a "$SETUP_LOG") 2>&1

fail() {
  echo "PATCH_FAIL_MSG:$1"
  echo "ERROR: $1"
  exit 1
}

require_file() {
  [ -f "$1" ] || fail "$2"
}

require_dir() {
  [ -d "$1" ] || fail "$2"
}

patch_textures() {
  [ -f "$TEXTURE_MARKER" ] && return 0

  require_dir "$GAMEDATA/data/textures" "Missing texture data. Copy the full Steam Windows install into ports/theswapper/gamedata."
  require_file "$TEXTURE_DOWNSCALER" "Port install is incomplete: missing texture downscaler."

  mkdir -p "$TEXTURE_STATE_DIR"
  "$TEXTURE_DOWNSCALER" "$GAMEDATA" "$NORMAL_MAP_MAX_DIMENSION" "$ATLAS_MAX_DIMENSION" || fail "Failed to downscale texture assets."
  {
    printf 'normal_map_max_dimension=%s\n' "$NORMAL_MAP_MAX_DIMENSION"
    printf 'atlas_max_dimension=%s\n' "$ATLAS_MAX_DIMENSION"
  } > "$TEXTURE_MARKER"
}

require_file "$GAMEDATA/TheSwapper.exe" "Missing TheSwapper.exe. Copy the Steam Windows files into ports/theswapper/gamedata."
require_file "$GAMEDATA/mainSettings.ini" "Missing mainSettings.ini. Copy the full Steam Windows install into ports/theswapper/gamedata."
require_dir "$GAMEDATA/data" "Missing data directory. Copy the full Steam Windows install into ports/theswapper/gamedata."
require_file "$GAMEDIR/config/TheSwapper.exe.config" "Port install is incomplete: missing TheSwapper.exe.config."

cp "$GAMEDIR/config/TheSwapper.exe.config" "$GAMEDATA/TheSwapper.exe.config"
patch_textures

if ! find "$PROFILE_DIR" -maxdepth 1 -type f -name "*.pro" | grep -q .; then
  cp "$GAMEDIR/savedata-seed/"* "$PROFILE_DIR/"
fi

touch "$SETUP_MARKER"
echo "Setup complete."
