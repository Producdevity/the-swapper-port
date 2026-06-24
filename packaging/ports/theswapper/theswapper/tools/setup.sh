#!/bin/bash
set -eu

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

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

TEXT_PATCH_MIN_TUTORIAL_SCALE="${TEXT_PATCH_MIN_TUTORIAL_SCALE:-1.35}"
TEXT_PATCH_PDA_MAP_HELP="NAVIGATOR - move stick - {key} closes"
TEXT_PATCH_PDA_TELEPORT_HELP="Choose target - {key} confirm - {key2} cancel"

NORMAL_MAP_MAX_DIMENSION="${NORMAL_MAP_MAX_DIMENSION:-512}"
NORMAL_MAP_STATE_DIR="$GAMEDIR/asset-patches/normal-maps"
NORMAL_MAP_MARKER="$NORMAL_MAP_STATE_DIR/downscaled.v1.${NORMAL_MAP_MAX_DIMENSION}"
NORMAL_MAP_MANIFEST="$NORMAL_MAP_STATE_DIR/manifest.txt"
NORMAL_MAP_DOWNSCALER="$GAMEDIR/tools/normalmap-downscale"

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

replace_key_value() {
  file="$1"
  key="$2"
  value="$3"

  [ -f "$file" ] || return 0

  tmp_file="${file}.tmp.$$"
  awk -v key="$key" -v value="$value" '
    BEGIN { prefix = key " = " }
    index($0, prefix) == 1 { $0 = prefix value }
    { print }
  ' "$file" > "$tmp_file" || {
    rm -f "$tmp_file"
    fail "Failed to patch $(basename "$file")."
  }

  if cmp -s "$file" "$tmp_file"; then
    rm -f "$tmp_file"
  else
    mv "$tmp_file" "$file"
  fi
}

patch_normal_maps() {
  texture_dir="$GAMEDATA/data/textures"
  manifest_tmp="${NORMAL_MAP_MANIFEST}.tmp.$$"
  patched_count=0

  [ -f "$NORMAL_MAP_MARKER" ] && return 0
  require_dir "$texture_dir" "Missing texture data. Copy the full Steam Windows install into ports/theswapper/gamedata."
  require_file "$NORMAL_MAP_DOWNSCALER" "Port install is incomplete: missing normal-map downscaler."

  mkdir -p "$NORMAL_MAP_STATE_DIR"
  : > "$manifest_tmp"

  while IFS= read -r asset; do
    rel_path="${asset#$GAMEDATA/}"

    case "$asset" in
      *.png)
        "$NORMAL_MAP_DOWNSCALER" "$asset" "$asset" "$NORMAL_MAP_MAX_DIMENSION" || {
          rm -f "$manifest_tmp"
          fail "Failed to downscale normal map: $rel_path"
        }
        ;;
      *.mips)
        rm -f "$asset"
        ;;
      *)
        continue
        ;;
    esac

    printf '%s\n' "$rel_path" >> "$manifest_tmp"
    patched_count=$((patched_count + 1))
  done < <(find "$texture_dir" -type f \( -name '*#normal*.png' -o -name '*#normal*.mips' \))

  mv "$manifest_tmp" "$NORMAL_MAP_MANIFEST"
  printf 'patched=%s\n' "$patched_count" > "$NORMAL_MAP_MARKER"
  echo "Downscaled normal maps to max ${NORMAL_MAP_MAX_DIMENSION}px for $patched_count assets."
}

patch_tutorial_text() {
  level_dir="$GAMEDATA/data/#sp#levels_sp"
  patch_state="$GAMEDIR/.tutorial_text_patch_v2"

  [ -f "$patch_state" ] && return 0
  require_dir "$level_dir" "Missing level data. Copy the full Steam Windows install into ports/theswapper/gamedata."

  patched_count=0
  for level_file in "$level_dir"/*.lvl; do
    [ -f "$level_file" ] || continue
    tmp_file="${level_file}.tmp.$$"

    awk -v min_scale="$TEXT_PATCH_MIN_TUTORIAL_SCALE" '
      function is_entity(line) {
        return line ~ /^ [A-Za-z_][A-Za-z0-9_]*[[:space:]]*$/
      }

      function flush_block(  i, line, parts, value) {
        if (block_lines == 0)
          return

        for (i = 1; i <= block_lines; i++) {
          line = block[i]
          if (patch_block && line ~ /^(xs|ys) -?[0-9.]+([eE][-+]?[0-9]+)?$/) {
            split(line, parts, " ")
            value = parts[2] + 0
            if (value > 0 && value < min_scale)
              line = parts[1] " " min_scale
          }
          print line
        }

        block_lines = 0
        patch_block = 0
      }

      {
        sub(/\r$/, "")
        if (is_entity($0))
          flush_block()

        block[++block_lines] = $0
        if ($0 ~ /^TextKey tuto_/)
          patch_block = 1
      }

      END {
        flush_block()
      }
    ' "$level_file" > "$tmp_file" || {
      rm -f "$tmp_file"
      fail "Failed to patch tutorial text in $(basename "$level_file")."
    }

    if cmp -s "$level_file" "$tmp_file"; then
      rm -f "$tmp_file"
    else
      mv "$tmp_file" "$level_file"
      patched_count=$((patched_count + 1))
    fi
  done

  touch "$patch_state"
  echo "Adjusted tutorial text scale in $patched_count level files."
}

patch_pda_text() {
  translation_dir="$GAMEDATA/data/translations/#sp#"
  patch_state="$GAMEDIR/.pda_text_patch_v1"

  [ -f "$patch_state" ] && return 0
  require_dir "$translation_dir" "Missing translation data. Copy the full Steam Windows install into ports/theswapper/gamedata."

  replace_key_value "$translation_dir/translations_sp.en" "pda_mapdescription_gamepad" "$TEXT_PATCH_PDA_MAP_HELP"
  replace_key_value "$translation_dir/translations_sp.en" "pda_teleportmapdescription_gamepad" "$TEXT_PATCH_PDA_TELEPORT_HELP"

  touch "$patch_state"
  echo "Adjusted PDA helper text for the handheld screen."
}

require_file "$GAMEDATA/TheSwapper.exe" "Missing TheSwapper.exe. Copy the Steam Windows files into ports/theswapper/gamedata."
require_file "$GAMEDATA/mainSettings.ini" "Missing mainSettings.ini. Copy the full Steam Windows install into ports/theswapper/gamedata."
require_dir "$GAMEDATA/data" "Missing data directory. Copy the full Steam Windows install into ports/theswapper/gamedata."
require_file "$GAMEDIR/config/TheSwapper.exe.config" "Port install is incomplete: missing TheSwapper.exe.config."

cp "$GAMEDIR/config/TheSwapper.exe.config" "$GAMEDATA/TheSwapper.exe.config"
patch_normal_maps
patch_tutorial_text
patch_pda_text

if ! find "$PROFILE_DIR" -maxdepth 1 -type f -name "*.pro" | grep -q .; then
  cp "$GAMEDIR/savedata-seed/"* "$PROFILE_DIR/"
fi

touch "$GAMEDIR/.setup_complete"
echo "Setup complete."
