# The Pro Street gameplay Lua API

This is a reference for people who write gameplay message handlers in the Sequence Builder. It covers Need for Speed Pro Street on PC, Lua 5.0. It says what a handler receives, which globals the game provides, what each function takes and returns, and where the game's code does something you would not expect.

It comes from three sources:

- The 251 handler chunks in `gameplay.lzc`, decompiled with `src/core/lua`. All of them decompile to source that compiles back to the same bytes.
- The PC retail executable `nfs.exe`, read with `tools/pc/dis.py`. Every binding, conversion rule and runtime detail below comes from its code. Addresses are virtual addresses in `nfs.exe`.
- The Xbox 360 milestone PDB (`tools/xex/symmap.py`), which names the C++ functions behind the bindings and gives their signatures.

The text marks how sure each statement is:

- "Verified", or an address, means I read it in `nfs.exe` code or data, or in the handler bytecode.
- "From the scripts" means it is how the retail handlers use something. The game accepts that use, which says little about what the function does inside.
- Descriptions of functions that no retail handler calls are guesses from their names. The function tables show "none" in the Retail column for those.

<!--SUMMARY-->

## The runtime

The game links the Lua 5.0 virtual machine, the binary chunk loader and part of the auxiliary library. It has no Lua parser. The error strings of the lexer and parser, such as `<eof>` and `unexpected symbol`, are absent, while those of the chunk loader, such as `bad signature in %s` and `unexpected end of file in %s`, are present. Handlers must be compiled before they go into the database. The Sequence Builder does that.

The game opens none of the standard libraries. The state is created at `0x6F5AC0`: `lua_open`, a panic function, and a metatable on the globals table whose `__index` is the game's global lookup. There is no `luaopen_*` call, and none of the library names exist as strings in the executable. So these are all missing:

- the base functions `print`, `type`, `tostring`, `tonumber`, `pairs`, `ipairs`, `next`, `unpack`, `assert`, `error`, `pcall`, `setmetatable`, `getmetatable`, `rawget`, `rawset`;
- the `string`, `table`, `math`, `io`, `os`, `coroutine` and `debug` tables.

Everything the virtual machine does by itself still works: arithmetic, comparisons, `and`, `or`, `not`, `..`, tables and their constructors, closures, `local function`, numeric `for`, `while` and `repeat`. `..` turns numbers into strings with `%.14g`, which is also the only way to convert a number to a string.

Some consequences:

- You cannot iterate a table by its keys. Lua 5.0's `for k, v in t do` looks up a global `next`, which does not exist. Loop over an array with a numeric `for` and keep its length yourself. The retail scripts keep counts in `vars` (`vars.NumRacers`, `vars.NumCheckpoints`).
- There is no `table.getn`, and Lua 5.0 has no `#` operator.
- The game adds two small helpers. `Table.Randomize(t)` shuffles an array in place, and `Math.RandomInt(n)` returns a whole number from 1 to n.
- Nothing prints. `Debug.Print` exists, but it is an empty function in the retail build. Hooking that function (`402CC0`, shared with `Debug.Assert`) makes both work.

### Errors end the game

Handlers run through `lua_call`, not `lua_pcall` (verified, `0x6EB450`). Nothing up the call chain sets an error handler. When a handler raises a Lua error, the game's panic function (`0x6F3830`) dumps the Lua stack and returns, and `luaD_throw` (`0x6E0610`) then calls `exit` from MSVCR80. The game quits to the desktop.

Any of these raises an error:

- calling a global or field that is nil, such as a misspelled function name;
- indexing nil, such as `message.Sender.X` when `Sender` is nil;
- arithmetic or `..` on nil or on a table;
- comparing a number with a string using `<`.

Check values before using them, the way the retail scripts do with `if not message.Sender then return end`. Calls with the wrong number of arguments do not raise errors. Missing arguments arrive as nil, extra ones are ignored. The section on argument conversion says what nil turns into.

### Lua heap

`Lua.ResetHeap()` asks the runtime to reset the Lua heap. Inside a handler it only sets a flag, and the reset happens when the message delivery ends (verified, `0x6E9340`). The 360 symbols call this `ResetHeapWhenSafe` and the snapshot `TakeResetSnapshot`. The retail basicrace script calls it when the countdown starts. What survives the reset is not verified.

## How a handler runs

A handler is a `messagehandler` collection of the `gameplay` class. `doc/gameplay.md` describes the state graphs. The game finds and runs one like this:

1. A Hermes message is sent somewhere in the game. For each of the 150 message classes that Lua can see, the game has a small handler that wraps the message in a `LuaMessageDeliveryInfo` and passes it to `LuaPostOffice::RouteMessage` (verified, for example `0x6C4D50` for `MNotifyEscaped`).
2. Each activity listening for that message kind runs `GActivity::HandleMessage` (`0x681990`). It walks the handlers it has for the message kind and keeps those whose `message_id` equals the message's `StringHash32`.
3. A handler can also have `FilterBlocks` and `FilterModePassAll`, which hold EventSys filter expressions (verified, `0x6580F0`). No retail handler sets them.
4. The first time a handler is needed, the game reads its `bytecode` blob, unpacks it with `LZDecompress`, loads it under the chunk name `"handler"` and runs it (verified, `0x657FF0`). Running the chunk defines the global function.
5. The game builds the `message` table, then `self`, `vars` and `ChangeState`, once per delivery.
6. It formats the function name with `"h_%08X"` and the handler collection's key, reads that global, and calls it with three arguments and no results (verified, `0x6581F0`).

So every handler chunk must define exactly this:

```lua
function h_8C272D2D(self, message, vars)
	-- 8C272D2D is StringHash32 of the handler collection's name
end
```

The parameter names are not stored in stripped chunks. `self, message, vars` is how World's `lithium.lua` source names them. The function's return values are ignored.

When several handlers of the same state handle the same message, all of them run, one after another. The basicrace `racing` state has nine `MPlayerStateEnter` handlers, and each starts by checking `message.State`. The order they run in is not verified.

Handlers belong to a state through `stateref`. `HandleMessage` finds them in a table the activity keeps per message kind. The scripts are written on the assumption that only the current state's handlers run, and the data is organized that way, but I did not trace the code that fills the table.

### State changes send MStateExit and MStateEnter

`GActivity::EnterState` sends `MStateExit` to the old state and `MStateEnter` to the new one through `GActivity::HandleLocalMessage` (verified, `0x681E20`, hashes `0x9C020C14` and `0x5E1AA4C2`). These two do not go through Hermes. The game passes no message and no table builder, so in `MStateEnter` and `MStateExit` handlers `message` is nil. `MStateEnter` is where the retail scripts set up `vars`.

### The three parameters

`self` is the activity whose state graph is running, as a `GRuntimeInstance` userdata (verified, `GActivity::BuildActivityTables` at `0x681920`). For a career race made from the `basicrace` template, `self` is the race element. `self.TrackLayout` reads that race's layout, and fields the race does not set come from its templates.

`message` is a fresh table with the message's fields, or nil. See [The message table](#the-message-table).

`vars` is a plain Lua table that belongs to the activity. The game keeps it in the Lua registry under a string taken from the activity, most likely its name, and creates it empty the first time (verified, `0x6818B0`). It survives between messages and state changes, so scripts use it for all their state. Anything can go in it, including userdata.

## ChangeState

```lua
ChangeState("done")
```

`ChangeState(name)` switches the activity to another state of its state graph (verified, `0x6669C0`). The name is the state's `Name` field, such as `"done"` for `stategraph_basicrace_done`. `GActivity::GetStateByName` (`0x666920`) compares it with each state's name. The game then allocates an `EChangeState` event with the activity's and the state's keys. The switch happens later, when the event system processes it, so the handler keeps running after the call. The retail scripts put `return` right after it when nothing else should happen.

`ChangeState` is a new global closure over the current activity, set before every delivery.

If no state has that name, `GetStateByName` returns null, and `ChangeState` then calls `Attrib::GetCollectionKey` on address 4. That reads a bad pointer and almost certainly crashes the game. Spell state names exactly.

The `transitionlist` field of a handler lists the states it can switch to. The Sequence Builder fills it from the `ChangeState` calls. The game's code that reads `transitionlist` was not traced.

Retail scripts call `ChangeState` <!--CHANGESTATE_COUNT-->.

## Instances: self and other elements

`self`, `message.Sender`, `self.TrackLayout`, `self.TrackLayout.Checkpoint[1]` and every other gameplay element reach Lua as `GRuntimeInstance` userdata. It holds a pointer to the game's runtime instance. Its metatable has three entries (verified, `LuaAttributes::BindAccessors`, `0x6F4390`).

Reading `instance.Name` (`__index`, `0x6F3CC0`):

1. The game hashes `Name` with `StringHash32` and reads that attribute of the instance's collection with `Attrib::Instance::Get`. Template inheritance applies, as in the editor's inspector.
2. If the instance has no such attribute, the game looks for a connected instance under that key and returns it as an instance.
3. Otherwise you get nil.

The attribute's type decides the Lua value. The game has accessors for 16 types (verified, table at `0xA61630`):

| AttribSys type | Lua value |
|---|---|
| `EA::Reflection::Float`, `Double`, `Int8` to `Int64`, `UInt8` to `UInt64` | number |
| `EA::Reflection::Bool` | boolean |
| `EA::Reflection::Text`, `Char`, `Attrib::StringKey` | string |
| `GCollectionKey`, `Attrib::RefSpec` | the element as an instance, or nil when it is not among this instance's connected instances |
| any other type: enums such as `GRace::Mode`, vectors, blobs, the map structs | nil |

An array attribute gives an array userdata (`LuaAttribArrayInfo`) instead of a value:

- `array.Count` is the number of elements.
- `array[i]` is element i, counting from 1, converted like a single value. An index below 1 gives nil. A string that reads as a number counts as a number.
- Any other key gives nil.

```lua
for i = 1, self.TrackLayout.Checkpoint.Count do
	RaceStatus:AddCheckpoint(self.TrackLayout.Checkpoint[i])
end
```

Writing `instance.Name = value` does nothing. The `__newindex` function (`0x6F3E10`) looks up the type's setter, and no type has one. Arrays ignore writes too. Use `vars` for your own data.

Comparing two instances with `==` uses `__eq` (`0x6F3F40`). They are equal when they are the same instance, or when one of them has a flag set and its parent collection is the other's collection. The meaning of that flag is not verified. Two arrays are equal when their instances are equal and they are the same attribute. Comparing an instance with nil is false, as in plain Lua.

Every read creates a new userdata. Keep the value in a local if you use it often.

<!--SELF_FIELDS-->

## The message table

The game builds the message table with a function generated per message class (verified, the builders listed in [All messages](#all-messages)). The keys are the class's field names without their `f` prefix: `fReason` becomes `message.Reason`.

| Field type in the header | Lua value |
|---|---|
| `int`, `unsigned int`, `float`, `unsigned long long` | number |
| `bool` | boolean |
| `char*` | string |
| `GCollectionKey` | the element as an instance, or nil when the game has no runtime instance for the key |
| `HSIMABLE__*` | an `ISimable` userdata, or nil when the handle is no longer valid |
| `unsigned int` car handles in `MHubCarChanged` and similar | a `GWrapper<T>` userdata, or nil |
| `UMath::Vector3`, `UMath::Vector4` | nil. The builder pushes nil for vectors. |

`MStateEnter` and `MStateExit` handlers get nil, as explained above. The 63 messages without fields share one builder (`0x6A19D0`) that makes an empty table, so when they come through Hermes `message` is `{}`.

The message classes and fields are the ones in `data/messages/prostreet_pc.txt`, minus nine that the PC build does not expose to Lua. [All messages](#all-messages) has all 150.

These are the messages the retail handlers react to, with the fields they read:

<!--MESSAGES_HANDLED-->

## Sending messages

Any global whose name starts with `M` and is the name of one of the 150 message classes is a function that sends that message:

```lua
MNotifyEscapeSafe("Gameplay", true)
MNotifyStagingEntered("Gameplay", message.Racer, self.StagingTime)
MRaceDone("Gameplay")
```

The game resolves these names on first use (verified). The globals table's `__index` (`0x6F38B0`) hashes a name that starts with `M`, finds it in a sorted table of 150 `{hash, function}` pairs (`0x98B8D0`), and stores the function in the globals table under that name.

The arguments:

- The first argument is the channel. The game hashes it with `UCrc32`. Retail scripts use `"Gameplay"` and `"Adapter"`.
- Then come the fields in the order the header declares them, which is the order in [All messages](#all-messages).
- The call needs exactly one argument more than the message has fields (verified for all 150 functions). With any other count it returns without sending anything. No error is raised.
- The message is queued with `Hermes::Message::Enqueue`, not sent at once (verified for all 150). Handlers that react to it run later, not inside the call. When the queue is processed was not traced.

Fields are converted like this (verified in the generated senders):

- Numbers go through `lua_tonumber`. Integer fields are truncated.
- Booleans go through `lua_toboolean`, so only nil and false are false.
- Strings go through `lua_tostring`.
- A simable field takes the handle stored in an `ISimable` userdata.
- A `GCollectionKey` field is always sent as 0. The generated code builds it from a null instance and ignores the argument (verified for `MNotifyRacePlacement`, `MTriggerEnter` and `MNotifySpeedTrap`). You cannot send a message that names an element.

A name that starts with `M` but is no message class falls through to the other lookups and is nil, and calling it is an error.

Retail scripts send these messages:

<!--MESSAGES_SENT-->

## Firing events

Any global whose name starts with `E` is a function that fires the EventSys event of that name (verified, `0x6EEBF0`):

```lua
EPlayRaceMovie(self.OutroMovie)
EEnterStagingWait(0)
EPlayRaceNIS(nil, self.IntroNIS, "Intro", 0, 0, nil, nil)
```

The global lookup gives every `E` name a closure over the name and stores it in the globals table. The closure does all the checks when it is called. It looks the event up by `StringHash32` of its name in the game's registry of 226 events (`0x6A7FF0`). It fires the event only when all of these hold, and otherwise returns without doing anything:

- the event exists;
- it is marked as callable from Lua (flag bit 1). 130 of the 226 events are;
- the number of arguments equals the number of fields;
- the fields fit in 256 bytes.

A misspelled event name therefore fails silently. `EBailPursuit` and `EShowPostPursuit`, which the retail pursuit scripts call, do not exist on PC, so those calls do nothing.

The arguments are the event's fields in order. They are converted by field type:

| Field type | Lua argument |
|---|---|
| float, double | number |
| int, uint and the 8, 16 and 64-bit integers | number, truncated |
| string | string. nil passes a null pointer. |
| hash | string, hashed with `EventSys::Hash32`. nil works and is what retail passes for "no marker". |

The event's field names come from the `f` members of its class in the debug header. All of them resolve.

Retail scripts fire these events:

<!--EVENTS_USED-->

[All events callable from Lua](#all-events-callable-from-lua) lists the 130 events.

## Global tables and functions

The game registers its functions in `LuaBindery::BindToGameCode` (`0x6F78F0`) and `LuaBindery::BindCustomMethods` (`0x6F5CE0`). It calls a typed binder for each, with the table name, the function name and the C function. I found every binder call site in the executable: 363 functions and methods, and 5 objects. Each binder installs a thunk that converts the Lua arguments for one C signature. Most binders have no name in the PC map, so I took each one's signature from the milestone symbols of the functions it binds. For every binder, all of those functions have the same signature, and the binders the PC map does name, such as `BindVoidFunction_PCc_f`, carry the same types in their names. A few functions have no milestone symbol; they get the signature of their binder.

A function marked "does nothing on PC", "always returns false" and so on is bound to a stub in `nfs.exe`. Its body is `ret`, `xor eax, eax; ret` or similar (verified). Several of these are things the retail scripts call, such as `Game.ForcePursuitStart` and `Debug.Print`.

Types in the signatures:

| Name | What it is |
|---|---|
| number, string, boolean, table | plain Lua values |
| instance | a `GRuntimeInstance` userdata: `self`, a trigger, a marker, any gameplay element |
| simable | an `ISimable` userdata: a car in the world |
| racerinfo | a `GRacerInfo` userdata, from `RaceStatus:GetRacerInfo` |
| handle | a `GWrapper<T>` userdata, from fields such as `MHubCarChanged.CarHandle` |

### Argument conversion

These rules come from the thunks (verified):

- A number parameter takes `lua_tonumber` of the argument. nil, a missing argument or a non-numeric string give 0. Integer parameters are truncated.
- A string parameter takes `lua_tostring`, so numbers become strings. nil gives a null pointer. What the C function does with a null string was not checked for each function. Pass a string.
- A boolean parameter is false for nil and false, and true for anything else, 0 included (`0x6E3C40`).
- An instance parameter takes the pointer stored in any userdata, without checking its type (`0x6E3B70`). Anything that is not userdata gives a null pointer, which is how `NIS.Play(nil, ...)` passes "no marker". Passing a simable where an instance is expected hands the C function a garbage pointer.
- A simable parameter reads the handle stored in a userdata and looks it up among the live simables (`0x6E72C0`). An invalid handle gives null.
- A handle parameter must be a userdata. A number, nil or missing argument makes the thunk read address 0, which crashes (`0x6E42E0`).

Return values:

- boolean, number and string as usual;
- an object pointer becomes a new userdata with the right metatable, or nil for null (`0x6EF290` for `GRacerInfo`);
- void functions return nothing.

<!--FUNCTIONS-->

## Objects and methods

Methods are called with `:`. The first argument is the object (verified, `0x6F2890`). The thunk takes the pointer out of whatever userdata is first and does not check its type. With `.` instead of `:` the method runs with a null object. Retail calls `RaceStatus.DoDDayReset()` that way, and it works only because that one function ignores its object. Always use `:`.

Five objects are globals set when the bindings are made (verified, binder `0x6F1BA0`). Three more are looked up each time a script reads the name, by the `DoCustomLookup` of their binder classes (verified). They are nil when there is nothing to return. `GHub` is nil outside a hub, for example.

<!--OBJECTS-->

The method metatables have only `__index`, pointing at the methods. Two userdata of these types never compare equal, even for the same object, because there is no `__eq` and every value is a new userdata. Compare indices instead, for example with `Game.GetRacerIndex` or `racerInfo:GetIndex()`.

<!--METHODS-->

## Constants

The game defines no constants for Lua. The retail scripts define their own tables in `vars`, in the `initial` state of `basicrace` and `org_monitor`. Their values match enums in the debug header, which makes them the numbers the game's functions expect (verified against `ProStreet08Milestone.h`):

<!--CONSTANTS-->

Retail passes `MNotifyRacerFinished.Reason` to `racerInfo:FinishRace`. Going by the names, both are a `GRacerInfo::FinishReason`, which the header defines as `kReason_Unknown` 0, `kReason_Completed` 2, `kReason_CrossedFinish` 6, `kReason_DidNotFinish` 10, `kReason_KnockedOut` 26, `kReason_Totalled` 42, `kReason_EngineBlown` 74, `kReason_Busted` 138, `kReason_ChallengeCompleted` 258, `kReason_Disconnected` 522, `kReason_FalseStart` 1034. No retail script uses these names.

## Names the retail scripts use that do not exist on PC

<!--MISSING-->

## Other mistakes in the retail scripts

These are worth knowing before copying a script. `speedtrap_activity`, `milestonerace` and `checkpointrace` have no retail elements, so their mistakes never show in the game. `speedtrap_group_monitor` is used, by the `speedtraps` element.

- `speedtrap_activity`'s `MTriggerEnter` handler calls `Game.NotifySpeedTrapTriggered(self, element, simableSpeedKmh)` with three arguments. The function takes an activity, a trap, a simable and a speed. The simable lands in the trap parameter and is read as an instance pointer. The other three callers pass four arguments.
- `milestonerace`'s `MPerpEscaped` handler tests `vars.MilestonePassed`. Its other handlers set `vars.PassedMilestone`, so the test is never true.
- `checkpointrace`'s `MTriggerEnter` handler compares with `self.Checkpoint` where basicrace uses `self.TrackLayout.Checkpoint`, passes the undefined global `newGoal`, and calls the missing `GetRacerIndex()`.
- `speedtrap_group_monitor` checks `Game.IsActiveSpeedTrap(sender)` first. That function always returns false on PC, so the handler never awards anything.
- Many scripts call `Debug.Print` and build strings for it. It does nothing in this build.

## What is still unknown

- The order in which several handlers for the same message run.
- Where the activity's per-message handler table is filled, and whether a race element's own handlers replace or add to its template's. Retail has one such element, `prostreet_career/bins/race_bin_ca_0_1/gr_1`, with two handlers of its own.
- What `Persistent` does. `GActivity::DeserializeVars` reads it, and 14 templates set it, true on `activity` and false on `race`, `hub` and `tier`. Whether it saves `vars` was not traced.
- What `Lua.ResetHeap` keeps, and whether instance userdata stored in `vars` can outlive the instance they point to. They hold raw pointers. Simable userdata hold handles and are safe.
- What `Debug.ShowScreenMessage` shows. It fires an event when a player car exists, but whether the retail build draws anything was not checked.
- The meaning of many numeric arguments: the ids of `Game.ShowLoadingScreen` and `Game.HideLoadingScreen`, the second number of `StateCheck`, `StateCheckAny` and `StateCheckFirst`, the arguments of `Game.NavigatePlayerTo` and `Game.SpawnCop`.
- What most functions that no retail script calls do. Their descriptions come from their names.
- Whether the C functions cope with null strings and null instances. Some check, most were not read.
- The type ids of message fields as stored in the game, except where their conversion was read.

## All messages

All 150 message classes the PC build exposes. "Received as" is what a handler finds in `message`. "Send with" is the argument list of the `M` function, with the Lua type each field takes. "Retail" counts the handlers that react to the message.

In the send column, "always 0" marks `GCollectionKey` fields, and "always zero" marks `UMath::Vector3` fields, which the sender never reads (verified for `MBreakerStopCops`). Pass anything, nil included, for these. "not usable" marks the car handle of `MHubCarChanged`, where the sender stores the pointer of `lua_tostring` of the argument. "not checked" marks the few other types whose sender I did not read.

<!--ALL_MESSAGES-->

## All events callable from Lua

The 130 events with flag bit 1 set, out of 226 registered. The other 96 are ignored when called from Lua.

<!--ALL_EVENTS-->

## Reproducing this

The scan scripts are in `scripts/lua_api/`. `scripts/lua_api/run_all.sh [work folder]` builds the repo's core libraries out of tree, rebuilds the dumps and writes this file, `lua-api.txt` and `api.json` into the work folder; copy them to `doc/lua-api.md` and `data/lua/prostreet_pc.txt`. It needs the `attrib/` databases, `nfs-data/nfs.exe` and the milestone image under `tools/xex/`.
