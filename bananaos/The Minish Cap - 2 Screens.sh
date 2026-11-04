#!/bin/bash
# The Legend of Zelda: The Minish Cap (Project Picori) with the second screen on the bottom panel. bananaOS / Anbernic RG DS.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
controlfolder="/roms/ports/PortMaster"
[ -d "$XDG_DATA_HOME/PortMaster/" ] && controlfolder="$XDG_DATA_HOME/PortMaster"
source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/tmc"
cd "$GAMEDIR"
chmod +x ./tmc_pc

# the menu keeps the bottom panel dark while a port runs; light it again
. /usr/libexec/banana/one-panel
one_panel_dark top
one_panel_restore

[ -f baserom.gba ] || cp "/$directory/gba/Legend of Zelda, The - The Minish Cap (USA).gba" baserom.gba
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL_VIDEODRIVER=kmsdrm SDL_VIDEO_DISPLAY_PRIORITY=DSI-2 BANANA_PANEL=1 SDL_AUDIODRIVER=alsa SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2048

./tmc_pc 2>&1 | tee ./log.txt
printf "\033c" >> /dev/tty1
