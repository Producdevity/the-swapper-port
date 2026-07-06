#!/bin/bash

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
# shellcheck source=/dev/null
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/theswapper"
GAMEDATA="$GAMEDIR/gamedata"
SAVEDIR="$GAMEDIR/savedata-home"
MONO_RUNTIME="mono-6.12.0.122-aarch64"
MONO_FILE="$controlfolder/libs/${MONO_RUNTIME}.squashfs"
MONO_DIR="$HOME/mono"

mkdir -p "$GAMEDATA" "$SAVEDIR"

run_setup() {
  if [ ! -f "$controlfolder/utils/patcher.txt" ]; then
    pm_message "This port requires a recent PortMaster release. Update PortMaster and try again."
    sleep 8
    exit 1
  fi

  if [ ! -x "$controlfolder/astcenc.aarch64" ]; then
    pm_message "This port requires PortMaster's ASTC encoder. Update PortMaster and try again."
    sleep 8
    exit 1
  fi

  export PATCHER_FILE="$GAMEDIR/tools/setup"
  export PATCHER_GAME="The Swapper"
  export PATCHER_TIME="a few minutes"
  export PATCHER_ASTC_ENCODER="$controlfolder/astcenc.aarch64"

  chmod +x "$PATCHER_FILE"
  # shellcheck source=/dev/null
  source "$controlfolder/utils/patcher.txt"
}

if [ ! -f "$GAMEDIR/.setup_complete" ]; then
  run_setup
fi

if [ ! -f "$GAMEDIR/.setup_complete" ]; then
  pm_message "Setup did not complete. Check ports/theswapper/setup.log."
  sleep 8
  exit 1
fi

if [ ! -f "$MONO_FILE" ]; then
  if [ -x "$controlfolder/harbourmaster" ]; then
    "$controlfolder/harbourmaster" --quiet --no-check runtime_check "${MONO_RUNTIME}.squashfs"
  else
    pm_message "Missing Mono runtime. Update PortMaster and try again."
    sleep 8
    exit 1
  fi
fi

mkdir -p "$MONO_DIR"
if [[ "${PM_CAN_MOUNT:-Y}" != "N" ]]; then
  $ESUDO umount "$MONO_DIR" >/dev/null 2>&1 || true
  $ESUDO mount "$MONO_FILE" "$MONO_DIR"
elif [ ! -x "$MONO_DIR/bin/mono" ] && ! command -v mono >/dev/null 2>&1; then
  pm_message "Mono is unavailable on this firmware."
  sleep 8
  exit 1
fi

cd "$GAMEDIR" || exit 1

>"$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export HOME="$SAVEDIR"
export PATH="$GAMEDIR/tools:$MONO_DIR/bin:$PATH"
export BROWSER="$GAMEDIR/tools/xdg-open"
export MONO_PATH="$GAMEDATA"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export MONO_MANAGED_WATCHER=1

BASE_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:${LD_LIBRARY_PATH:-}"

if [ -f "${controlfolder}/libgl_${CFW_NAME}.txt" ]; then
  # shellcheck source=/dev/null
  source "${controlfolder}/libgl_${CFW_NAME}.txt"
else
  # shellcheck source=/dev/null
  source "${controlfolder}/libgl_default.txt"
fi

PORT_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

if [[ "${DEVICE_ARCH}" != "x86_64" && (-n "${LIBGL_FB:-}" || -n "${LIBGL_ES:-}") ]]; then
  export SDL_VIDEO_GL_DRIVER="$GAMEDIR/gl4es.${DEVICE_ARCH}/libGL.so.1"
  export SDL_VIDEO_EGL_DRIVER="$GAMEDIR/gl4es.${DEVICE_ARCH}/libEGL.so.1"
fi

if [ -n "$BASE_LD_LIBRARY_PATH" ]; then
  export LD_LIBRARY_PATH="$BASE_LD_LIBRARY_PATH"
else
  unset LD_LIBRARY_PATH
fi

cd "$GAMEDATA" || exit 1

$GPTOKEYB "mono" &
pm_platform_helper "$MONO_DIR/bin/mono"

ASTC_CACHE_DIR="$GAMEDIR/asset-patches/astc"
ASTC_MANIFEST="$ASTC_CACHE_DIR/manifest.tsv"
ASTC_LIB="$GAMEDIR/libs.${DEVICE_ARCH}/libtexture_astc.so"
GAME_LD_PRELOAD="${LD_PRELOAD:-}"
if [ -f "$ASTC_MANIFEST" ] && [ -f "$ASTC_LIB" ]; then
  if [ -n "$GAME_LD_PRELOAD" ]; then
    GAME_LD_PRELOAD="$ASTC_LIB:$GAME_LD_PRELOAD"
  else
    GAME_LD_PRELOAD="$ASTC_LIB"
  fi
fi

run_game() {
  if [ -n "${TASKSET:-}" ]; then
    ${TASKSET} env \
      LD_LIBRARY_PATH="$PORT_LD_LIBRARY_PATH" \
      LD_PRELOAD="$GAME_LD_PRELOAD" \
      SWAPPER_ASTC_CACHE_DIR="$ASTC_CACHE_DIR" \
      SWAPPER_ASTC_MANIFEST="$ASTC_MANIFEST" \
      mono TheSwapper.exe "$@"
  else
    env \
      LD_LIBRARY_PATH="$PORT_LD_LIBRARY_PATH" \
      LD_PRELOAD="$GAME_LD_PRELOAD" \
      SWAPPER_ASTC_CACHE_DIR="$ASTC_CACHE_DIR" \
      SWAPPER_ASTC_MANIFEST="$ASTC_MANIFEST" \
      mono TheSwapper.exe "$@"
  fi
}

if [ -f "$GAMEDIR/memory-profiling.enabled" ]; then
  profile_dir="$GAMEDIR/logs/memory-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$profile_dir"
  echo "Writing memory profile to $profile_dir"

  run_game &
  game_pid="$!"

  "$GAMEDIR/tools/mem-profile" "$game_pid" "$profile_dir" 1 &
  profiler_pid="$!"

  wait "$game_pid"
  status="$?"
  kill "$profiler_pid" >/dev/null 2>&1 || true
else
  run_game
  status="$?"
fi

if [[ "${PM_CAN_MOUNT:-Y}" != "N" ]]; then
  $ESUDO umount "$MONO_DIR" >/dev/null 2>&1 || true
fi

pm_finish
exit "$status"
