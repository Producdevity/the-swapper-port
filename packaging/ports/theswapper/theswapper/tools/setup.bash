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
NORMAL_MAP_STATE_DIR="$GAMEDIR/asset-patches/normal-maps"
NORMAL_MAP_MARKER="$NORMAL_MAP_STATE_DIR/downscaled-${NORMAL_MAP_MAX_DIMENSION}"
NORMAL_MAP_DOWNSCALER="$GAMEDIR/tools/normalmap-downscale"
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

patch_normal_maps() {
  [ -f "$NORMAL_MAP_MARKER" ] && return 0

  require_dir "$GAMEDATA/data/textures" "Missing texture data. Copy the full Steam Windows install into ports/theswapper/gamedata."
  require_file "$NORMAL_MAP_DOWNSCALER" "Port install is incomplete: missing normal-map downscaler."

  mkdir -p "$NORMAL_MAP_STATE_DIR"
  "$NORMAL_MAP_DOWNSCALER" "$GAMEDATA" "$NORMAL_MAP_MAX_DIMENSION" || fail "Failed to downscale normal maps."
  printf 'max_dimension=%s\n' "$NORMAL_MAP_MAX_DIMENSION" > "$NORMAL_MAP_MARKER"
}

set_profile_value() {
  profile="$1"
  key="$2"
  value="$3"

  if grep -q "^${key} " "$profile"; then
    sed -i "s#^${key} .*#${key} ${value}#" "$profile"
  else
    printf '%s %s\n' "$key" "$value" >> "$profile"
  fi
}

apply_handheld_profile_defaults() {
  for profile in "$PROFILE_DIR"/*.pro; do
    [ -f "$profile" ] || continue

    set_profile_value "$profile" "MaxAudioChannels" "24"
    set_profile_value "$profile" "ShadowQuality" "0"
    set_profile_value "$profile" "DiffuseQuality" "0"
    set_profile_value "$profile" "ParticleQuality" "0"
    set_profile_value "$profile" "IsFilmlookEnabled" "False"
    set_profile_value "$profile" "IsPostFxEnabled" "False"
    set_profile_value "$profile" "PostFxBlurQuality" "0"
    set_profile_value "$profile" "PostFxMotionBlurQuality" "False"
    set_profile_value "$profile" "PostFxBloomQuality" "0"
    set_profile_value "$profile" "LightShaftQuality" "0"
    set_profile_value "$profile" "GBufferSampleAntialising" "0"
    set_profile_value "$profile" "Dithering" "False"
    set_profile_value "$profile" "Vignette" "0"
    set_profile_value "$profile" "HiResEdges" "False"
    set_profile_value "$profile" "IsVsyncEnabled" "False"
    set_profile_value "$profile" "XResolution" "640"
    set_profile_value "$profile" "YResolution" "480"
    set_profile_value "$profile" "SmoothSampling" "False"
    set_profile_value "$profile" "Antialising" "False"
  done
}

require_file "$GAMEDATA/TheSwapper.exe" "Missing TheSwapper.exe. Copy the Steam Windows files into ports/theswapper/gamedata."
require_file "$GAMEDATA/mainSettings.ini" "Missing mainSettings.ini. Copy the full Steam Windows install into ports/theswapper/gamedata."
require_dir "$GAMEDATA/data" "Missing data directory. Copy the full Steam Windows install into ports/theswapper/gamedata."
require_file "$GAMEDIR/config/TheSwapper.exe.config" "Port install is incomplete: missing TheSwapper.exe.config."

cp "$GAMEDIR/config/TheSwapper.exe.config" "$GAMEDATA/TheSwapper.exe.config"
patch_normal_maps

if ! find "$PROFILE_DIR" -maxdepth 1 -type f -name "*.pro" | grep -q .; then
  cp "$GAMEDIR/savedata-seed/"* "$PROFILE_DIR/"
fi

apply_handheld_profile_defaults

touch "$SETUP_MARKER"
echo "Setup complete."
