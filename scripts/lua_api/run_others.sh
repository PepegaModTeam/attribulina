#!/bin/sh
# Rescans what Carbon's PC executable, Undercover's Wii executable, current-gen Undercover's Xbox 360 debug
# and PC executables and Most Wanted's PC, GameCube and Xbox executables give gameplay Lua and writes
# carbon_pc.txt, undercover_wii.txt, undercover_pc.txt and mw_pc.txt (the format of data/lua/prostreet_pc.txt)
# into an output folder.
#   scripts/lua_api/run_others.sh <work folder> <output folder>
#   e.g. scripts/lua_api/run_others.sh /tmp/lua_others data/lua
# The work folder gets the scans (*.tsv), a report per game (what came from Pro Street's list, what
# differs, the stub notes), mw_builds.txt and uc_builds.txt (how Most Wanted's and current-gen Undercover's
# builds differ). Nothing else is written.
# Needs python3 with capstone, and from the repo:
#   exes/pc/carbon/NFSC.exe + NFSC.map (tools/pc reads the identical copy in nfs-data/carbon/pc-release),
#   nfs-data/carbon/wii-release/NfsMWRelease.elf (types for PC binders the map doesn't name),
#   exes/wii/uc-vanilla/NfsMWEuropeEnDuRelease.elf,
#   field names: data/messages/*.txt, data/lua/prostreet_pc.txt, nfs-data/carbon/ps3-milestone/types_pahole.txt,
#   nfs-data/SpeedDebugOpt-enUS.h,
#   current-gen Undercover: nfs-data/SpeedDebugOpt-enUS.exe + .pdb (the 360 debug build's image, which
#   tools/xex/extract_image.py --undercover also makes), exes/pc/undercover/nfs_steam.exe + nfs_steam.map,
#   Most Wanted: exes/pc/mw/speed.exe + speed.map, nfs-data/mw/gc-release/NFSMWRELEASE.ELF (types, event
#   arguments) with exes/gamecube/mw-vanilla/NFSMWRELEASE.h (event field names), exes/xbox/mw-vanilla/default.xbe
#   + default.xbe.map, and the PS2 builds' strings (exes/ps2/mw-beta/NFS.ELF, nfs-data/mw/ps2-milestone/NFS.ELF).
set -e
S=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$S/../.." && pwd)
W=${1:?usage: run_others.sh <work folder> <output folder>}
O=${2:?usage: run_others.sh <work folder> <output folder>}
mkdir -p "$W" "$O"
PY="python3 -I -B"
PS="$R/data/lua/prostreet_pc.txt"

# Carbon: the PC build, with the Wii build for the types of binders NFSC.map leaves unnamed
CNAMES="$R/data/messages/carbon_pc.txt $R/data/messages/prostreet_pc.txt $PS $R/nfs-data/carbon/ps3-milestone/types_pahole.txt"
$PY "$S/scan_carbon_pc.py" "$R" "$W/carbon_pc_bindings.tsv" "$W/carbon_pc_events.tsv" $CNAMES
$PY "$S/scan_wii.py" "$R/nfs-data/carbon/wii-release/NfsMWRelease.elf" \
    "$W/carbon_wii_bindings.tsv" "$W/carbon_wii_events.tsv" "$W/carbon_wii_messages.tsv" $CNAMES
$PY "$S/write_list.py" "$O/carbon_pc.txt" --game Carbon --platform PC --exe exes/pc/carbon/NFSC.exe \
    --bindings "$W/carbon_pc_bindings.tsv" --types-from "$W/carbon_wii_bindings.tsv" \
    --types-from-name "the Wii build (nfs-data/carbon/wii-release/NfsMWRelease.elf)" \
    --events "$W/carbon_pc_events.tsv" --prostreet "$PS" --report "$W/carbon_pc_report.txt"

# Undercover: the Wii build. The channels are the ones its retail handlers pass to M<Name>.
UNAMES="$R/data/messages/undercover_pc.txt $R/data/messages/carbon_pc.txt $R/data/messages/prostreet_pc.txt $PS $R/nfs-data/SpeedDebugOpt-enUS.h"
$PY "$S/scan_wii.py" "$R/exes/wii/uc-vanilla/NfsMWEuropeEnDuRelease.elf" \
    "$W/undercover_wii_bindings.tsv" "$W/undercover_wii_events.tsv" "$W/undercover_wii_messages.tsv" $UNAMES
$PY "$S/write_list.py" "$O/undercover_wii.txt" --game Undercover --platform Wii \
    --exe exes/wii/uc-vanilla/NfsMWEuropeEnDuRelease.elf \
    --bindings "$W/undercover_wii_bindings.tsv" --events "$W/undercover_wii_events.tsv" \
    --messages "$W/undercover_wii_messages.tsv" --channels "Gameplay,Camera,CameraMessagePort,AICopManager" \
    --prostreet "$PS" --report "$W/undercover_wii_report.txt"

# Undercover, current-gen (PC and Xbox 360; Lua 5.1): the 360 debug build, which has symbols, for the bindings
# and their types, with the PC build's stubs, events and MessageBindables (scan_uc360.py compares the two)
$PY "$S/scan_uc360.py" "$R" "$W" $UNAMES
$PY "$S/write_list.py" "$O/undercover_pc.txt" --game Undercover --platform PC \
    --exe "the Xbox 360 debug build (nfs-data/SpeedDebugOpt-enUS.exe and its PDB) and the PC build (exes/pc/undercover/nfs_steam.exe)" \
    --binders "LuaBindery::BindToGameCode, BindCustomMethods and the LuaFuncBinderProxy objects bind, the \
ILuaBindable classes bind or look up" \
    --bindings "$W/uc_bindings.tsv" --events "$W/uc_events.tsv" --constants "$W/uc_constants.tsv" \
    --messages "$W/uc_messages.tsv" --channels "Gameplay,Adapter" \
    --note "The PC and Xbox 360 versions run Lua 5.1 and nearly the same handlers. No retail 360 executable is at \
hand, so the 360 debug build stands in for it: its symbols give the names and types. The PC build binds the same \
names, numbers and messages, and the same events plus ECancelAutoSaveRequest; scripts/lua_api/scan_uc360.py compares \
them (uc_builds.txt in its work folder). The stub notes are the PC's, and a line says so where the 360 debug build \
differs. The E<Name> lines come from the PC's event definitions." \
    --note "The retail handlers also use names that neither build binds, so they are not listed: Most Wanted's \
Demo.SetRaceCompleteForFE and Demo.StorePursuitRepForFE; Pro Street's GTier (IncrementConsecutiveRacesWon, \
IncrementConsecutiveRacesLost), GHub:IsCareer, GRacerInfo:SetTimer, Game.AwardPlayerBounty, Game.ShouldHideSpeedTrap, \
Game.WarpPlayerAndWingmanToMarker and MiniMap.AddEngagedRace; and EShowPostPursuit, GRaceType.TakeDown and \
Game.GetSimableHealth." \
    --shared-functions --prostreet "$PS" --report "$W/undercover_pc_report.txt"

# Most Wanted: the PC build, with the GameCube build for the types (no binder has a name in speed.map) and
# for the events' arguments, and the Xbox build compared (scan_mw.py)
$PY "$S/scan_mw.py" "$R" "$W"
$PY "$S/write_list.py" "$O/mw_pc.txt" --game "Most Wanted" --platform PC --exe exes/pc/mw/speed.exe \
    --binders "LuaBindery::BindToGameCode binds" --events-text "events that have a Lua binding" \
    --bindings "$W/mw_pc_bindings.tsv" --types-from "$W/mw_gc_bindings.tsv" \
    --types-from-name "the GameCube build (nfs-data/mw/gc-release/NFSMWRELEASE.ELF)" \
    --note "The E<Name> argument names are the events' fields in the GameCube build's debug header \
(exes/gamecube/mw-vanilla/NFSMWRELEASE.h). The Xbox build (exes/xbox/mw-vanilla/default.xbe) binds the same \
names and events; scripts/lua_api/scan_mw.py compares them (mw_builds.txt in its work folder)." \
    --events "$W/mw_pc_events.tsv" --shared-functions --prostreet "$PS" --report "$W/mw_pc_report.txt"
