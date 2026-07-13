#!/bin/bash
# PortMaster launcher — Animal Crossing (GameCube decomp port)
# Target: Anbernic RG-34XX SP and other armhf PortMaster devices.

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

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/ac-gc"
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR" "$GAMEDIR/rom"

cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# First-run settings tuned for 720x480 handheld (Mali-G31): fullscreen,
# no MSAA, vsync on, dynamic FPS target. The in-game settings menu can
# change all of these afterwards.
if [ ! -f "$GAMEDIR/settings.ini" ]; then
cat > "$GAMEDIR/settings.ini" <<'EOF'
[Graphics]
window_width = 720
window_height = 480
fullscreen = 1
vsync = 1
msaa = 0
[Performance]
fps_target = 6
particle_quality = 2
EOF
fi

export XDG_DATA_HOME="$CONFDIR"
export LD_LIBRARY_PATH="/usr/lib32:$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
# muOS audio is PipeWire; 32-bit clients need the lib32 plugin paths set
# explicitly or pw_loop_new fails with "can't make support.system handle".
[ -d /usr/lib32/spa-0.2 ] && export SPA_PLUGIN_DIR=/usr/lib32/spa-0.2
[ -d /usr/lib32/pipewire-0.3 ] && export PIPEWIRE_MODULE_DIR=/usr/lib32/pipewire-0.3
export SDL_AUDIODRIVER=pipewire,alsa,dsp

# Audio diagnostics for log.txt while we chase the silence bug
echo "--- audio diag ---"
ls /usr/lib32 2>/dev/null | grep -iE "spa|pipewire|asound|pulse|SDL2"
ls /usr/lib32/spa-0.2/support 2>/dev/null
cat /proc/asound/cards 2>/dev/null
echo "--- end diag ---"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
chmod +x "$GAMEDIR/AnimalCrossing"

$GPTOKEYB "AnimalCrossing" &
pm_platform_helper "$GAMEDIR/AnimalCrossing"
./AnimalCrossing

pm_finish
