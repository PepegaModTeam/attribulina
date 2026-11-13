# The gameplay database

Pro Street keeps its activities, races, tracks, characters, hubs and gameplay scripts in one AttribSys class, `gameplay`. `gameplay.lzc` holds it: 5154 collections in 286 vaults. On top of AttribSys it has its own way of organizing things, and the editor's gameplay view (View > Gameplay View) follows it. `src/core/Gameplay.h` implements it.

Everything below was checked against Pro Street PC retail data. The counts come from that data.

## gameplay.lzc and gameplay.bin

The game loads `gameplay.lzc` whole when it starts, for the vault directory and the vaults that stay loaded (`gpcore` holds the templates and scripts). Every other vault is read from `gameplay.bin` when it's needed, at the offsets in the `.lzc`'s directory. So the two files must be the same pack. An edit that grows `gpcore` moves every vault after it, and an old `gameplay.bin` beside a new `gameplay.lzc` crashes the game as soon as it streams one in (`doc/vault-format.md` has the addresses). Saving `gameplay.lzc` writes `gameplay.bin` too. Opening a folder where they differ logs a warning and marks the pack unsaved, so Save All puts them back in step. Copy both files into the game.

## Templates

A collection's AttribSys parent is its template. Templates inherit from each other up to `baseelement`:

```
baseelement > activity > race > engaged > basicrace > circuit
baseelement > item > track_layout
baseelement > character
```

Template collections have short names with no `/`, and their `Template` field is set. An element made from a template sets `Template` to false.

## Folders and Children

Element names are paths: `prostreet_career/bins/race_bin_an_2_1/gr_1`, `tracks/autobahn_gd/autobahn_gd_grip_1`. Almost every path prefix is an element too, a folder (`tracks/autobahn_gd`, template `race_track`). Only 7 prefixes have no element.

An element lists the elements under it in `Children` (`GCollectionKey[]`). 4802 listed children exist and 57 entries point at collections that don't. An element's owner is the element whose `Children` lists it. When none does, the editor uses the element at its path's parent.

Every element sets `Name` and `CollectionName` to its own path. Retail breaks this in a few places: `Name` is `character` on some characters, and 2 `CollectionName`s name another element. The editor keeps them as they are.

Paths and `Children` lists agree in Pro Street's retail files: no element lists one outside its own path. Two paths have no element of their own, `tracks_prototype` and `tracks/chicago_gd/chicago_gd_drift_2`, with elements under them. A folder's Add to Script > With Everything Under It (`GameplayFolderContents`) takes the elements under its path, then whatever their lists name, so a mod's list that names elements elsewhere comes along too. The script also makes the vaults they are kept in and sets the owner's `Children`, so it rebuilds the folder in a database without it (`doc/scripts.md`).

Two names can share a StringHash32. The name list has Carbon's `race_bin_exotic/4_5_2/shortcut06`, whose hash Pro Street's `tracks/leipzig/leipzig_grip_1_gp` has too, and Carbon's `characters/major_crew_darius/alan`, whose hash is Pro Street's `tracks/portland/portland_grip_1/checkpoint1`. An element's `CollectionName` holds its name, and a string the vaults hold names its hash over the list's (`KeyNames::addFromData`), so Pro Street shows its own. Before that, the track layout showed as Carbon's race, and this page took it for a race listing a track's checkpoints.

## gameplayvault

`gameplayvault` (`Text`) names the vault that holds an element's children, and the game loads that vault by name. In retail, all 4802 listed children are stored in their owner's `gameplayvault` vault. For example, `tracks/autobahn_gd` (stored in `gpcore`) has `gameplayvault` `autobahn_gd`, and its layouts live in vault `autobahn_gd`. `tracks/autobahn_gd/autobahn_gd_grip_1` has `autobahn_gd_grip_1`, which holds its checkpoints, start grid and finish line.

728 elements set `gameplayvault`. 435 of those names have no vault: races name one (`an_2_1_gr_1`) but have no children. Every vault in the pack is some element's `gameplayvault`. A vault's `DepN` lists `<name>.vlt` and `<name>.bin`.

The naming follows the siblings. A track layout's vault is its own last path part (`autobahn_gd_grip_1`). A race's is the bin's suffix plus its own (`an_2_1_gr_1` for `race_bin_an_2_1/gr_1`). The editor suggests names the same way.

The 4426 elements that set no `gameplayvault` of their own inherit `gpcore` from `baseelement`, the root template, and the element header shows that.

New Element Here makes the vault when its first child goes in. The Explorer's Files tab makes, renames and deletes vaults directly (a pack's and a vault's right-click menus). Renaming a vault changes every `gameplayvault` that named it. Deleting one takes its collections, and those made from them, with it, but leaves the `Children` lists and `gameplayvault` values that name them.

World keeps each event in a file of its own, `gc.vaults/<event>.bin`, and the event's element sets `gameplayvault` to its own vault (`race_bin_10/10_2_2_sprint` in `10_2_2_sprint.bin`, with its children in the same file). The Files tab's menu makes a new one (New Event Vault File); saving writes it.

## References

`GCollectionKey` fields hold `StringHash32` of a collection name. Of 8,394 resolvable values, all but `SunAndSky` point at gameplay elements. `SunAndSky` points at the `sunandsky` class (714 values). 63 values name nothing that is loaded.

A few `Text` fields hold element paths. `TrackRecordHolder` does in all 484 values (`prostreet_career/opponents/grip_king`); `TrafficPattern` in its one value. The editor treats a `Text` field as a reference when at least 90% of its values are paths. `Name` and `CollectionName` don't count.

The reference picker learns from the data. A field offers the elements made from the templates its existing values use (`TrackLayout` offers `track_layout`s), and "All elements" lifts that filter.

## State graphs and scripts

An activity template (`basicrace`) has states and message handlers:

- A state is a `state` collection named `stategraph_<activity>_<state>` (`stategraph_basicrace_racing`), with `templateref` = the activity.
- A handler is a `messagehandler` collection named `<activity>/stategraph_<activity>_<state>_handler_<message>`, with suffixes `_2`, `_10` when one state handles a message more than once. Its fields:
  - `handler_owner`: the activity;
  - `stateref`: the state;
  - `message_id`: `StringHash32` of the Hermes message class (`MNotifyEscaped` = `0x72E0831B`);
  - `bytecode`: compiled Lua in an LZHeader block;
  - `transitionlist`: the states it can switch to.

How the game puts them together (`GActivity` in Pro Street's 360 milestone):

- A state belongs to a running activity when its `templateref` names the activity or one of its templates (`CollectionIsStateForActivity` follows the activity's parents). So an activity runs its templates' states too. Carbon's and the Wii's Undercover's `stategraph_career_game_initial` has `templateref` = `career`, and `career_game`, a template under `career`, runs it as its `initial`. The race templates under `basicrace` have no states of their own and run `basicrace`'s.
- `ChangeState( "x" )` finds the state by its `Name` field (`GetStateByName` compares the first member of the gameplay layout).
- A handler counts for a state when its `stateref` is the state and its `handler_owner` is the activity, an element with the same `CollectionName`, or one of the activity's templates (`CollectionIsHandlerForState`). The race modes under `basicrace` run its states and own handlers of them: in Pro Street `challengerace` and `sectorshootout` six each, `driftrace` five, `lapknockout` one, and some races (`prostreet_career/bins/race_bin_ca_0_1/gr_1`) a few of their own. A handler of `basicrace`'s state that `lapknockout` owns runs only in `lapknockout` races (`GameplayModel::handlersOf(state, activity)`, `handlerCountsFor`).
- States also have a `layoutpos`, probably where Black Box's graph editor placed them (inferred).

The State Graph Builder shows a template's own states and the ones it inherits (`GameplayModel::allStatesOf`), named by `Name`. Its Activity list has every template with states, its own or its templates', and every element that owns handlers, as a tree by parent (`GameplayModel::graphActivities`): `lapknockout` sits under `circuit`, under `basicrace`. A state lists the handlers that count for the activity shown, and links to the other activities that own handlers of it. A handler's right-click menu in that list edits its code, opens it, or deletes it. Deleting one of a template's handlers (`basicrace`'s, shown in `lapknockout`) takes it from every activity under the template, so the prompt names them first. A handler another collection is made from can't be deleted until that one goes.

Each handler's Lua defines one global, `h_` plus `StringHash32` of the handler's name in uppercase hex (in Most Wanted, the handler's name itself, which the game finds through its `CollectionName`; its handlers are named `<state>_handler_<message>`, with no activity in front). It takes `(self, message, vars)` and switches state with `ChangeState( "<state>" )`. The message's fields are `message.<Field>`. The games share message names. `scripts/gen_messages.py` lists each game's message classes and their fields from its debug header: `data/messages/prostreet_pc.txt` (159), `carbon_pc.txt` (97), `undercover_pc.txt` (214, the PC game's) and `mw_pc.txt` (66, from `NFSMWRELEASE.h`). Together they name every message id the handlers of Pro Street (65) and Carbon (46) use, and all but one of Most Wanted's 39 (`MCareerIntroDone`, inferred below). Undercover's Wii version has 19 more that no header has; `scripts/infer_messages.py` found their names from the handlers' names (`..._handler_notifyroundover` answers `MNotifyRoundOver`) and checked each against the id it should hash to. They are in `data/messages/inferred.txt`, without fields. The editor loads every list. The State Graph Builder shows one entry per message, the open game's own where it has one.

The game packs `bytecode` with `LZCompress`, which tries HUFF and JDLZ and keeps JDLZ only when it is strictly smaller (235 HUFF, 16 JDLZ in retail; the editor's re-encoding matches 246 of the 251 blocks byte for byte). `doc/lua.md` describes the chunks.

## Making new races and track layouts

The gameplay view's commands keep these rules.

- **New Element Here.** Creates an element from a template, listed in the owner's `Children` and stored in the owner's `gameplayvault` vault (the vault is created when missing). It also sets `Name`/`CollectionName` and optionally the element's own `gameplayvault`.
- **Duplicate.** Copies an element, with everything under it if asked, to a new path.
  - Paths under it and the vaults its elements own are renamed by replacing the old last path part with the new one (`autobahn_gd_grip_1` to `autobahn_gd_grip_9`).
  - References inside the copy (`racestart`, `Checkpoint`, ...) point at the copies.
  - Copied message handlers are recompiled with their new function name: decompile, rename `h_...`, compile.
- **Delete.** Removes an element and everything under it, takes it out of its owner's `Children`, and drops vaults that end up empty. References to it elsewhere are left alone.
