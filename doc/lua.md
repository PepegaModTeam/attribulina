# Lua in the gameplay database

The games' race and activity logic runs as small Lua functions. Each one sits
in the `bytecode` field (`Attrib::Blob`) of a `gameplay` collection, compiled.
Pro Street PC has 251 of them. `src/core/lua/Lua.h` is the API: reading,
listing (`Disassemble`), `Inspect`, `Decompile` and `Compile`.

`doc/lua-api.md` describes what the scripts can call: how the game runs a handler, the functions and objects it gives them, and what is missing (no standard library at all, and a Lua error ends the game).

Two Lua versions are in use, and both are implemented. Most Wanted, Carbon,
Pro Street and the Wii and PS2 Undercover (a Carbon-engine game) use Lua 5.0.
Undercover on PC and 360 and World use Lua 5.1. Each version has its own
reader, writer, listing, compiler and decompiler. `Inspect`, `Disassemble`
and `Decompile` go by the chunk's header (`DetectVersion`), so a 5.0 chunk
takes exactly the 5.0 path. `Compile` takes the version from the caller, who
picks it by the database's game and the chunks already there.

## Where the chunks are

A blob holds an LZHeader block, 235 HUFF and 16 JDLZ in Pro Street, which
`Compression.h` unpacks. Unpacked, it's a complete Lua chunk. Each chunk defines one global
function and nothing else:

```lua
function h_8C272D2D(self, message, vars)
    ...
end
```

The name is `HandlerFunctionName(collection name)`, `"h_"` plus
`StringHash32` of the `messagehandler` collection's name in 8 uppercase hex
digits. That hash is the collection's key, so the name can be checked without
knowing the collection's name. Most Wanted names the function as the handler
collection itself (`stategraph_basicrace_prerace_handler_stateenter_3`), whose
`StringHash32` is the key; the game looks it up by the handler's
`CollectionName` field. `HandlerFunction` (Gameplay.h) picks the rule by game. World's
`lithium.lua` shows how the authoring tool wrote these files: `self`,
`message` and `vars` as parameters, and generated
`--[[<escape class="TransitionGenerator" ...>]]` blocks around
`ChangeState(...)` calls.

## Chunk format (Lua 5.0, game variant)

The layout is stock Lua 5.0 `ldump.c`, built for a 32-bit little-endian
machine with `lua_Number` set to `float`. Every platform uses this layout,
the big-endian consoles included, so stock `luac` (double numbers) can't
read the files.

```
header     1B 'L' 'u' 'a'   signature
           50               version 5.0
           01               little-endian
           04 04 04         sizeof int, size_t, Instruction
           06 08 09 09      bits of OP, A, B, C
           04               sizeof lua_Number
           float 3.14159265358979323846e7   test number (bytes 3B AF EF 4B)
function   string source    u32 size including the NUL; 0 means NULL
           i32  lineDefined
           u8   nups, numparams, is_vararg, maxstacksize
           i32 n, n x i32                        line info
           i32 n, n x (string, i32, i32)         locals: name, startpc, endpc
           i32 n, n x string                     upvalue names
           i32 n, n x (u8 type, value)           constants: 0 nil, 3 float, 4 string
           i32 n, n x function                   nested functions
           i32 n, n x u32                        code
```

Retail chunks went through `luac -s`. That drops line info, local names and
upvalue names, and sets every source to `"=(none)"`. `ldump.c` then writes
NULL for a nested function's source because it equals its parent's. The
compile keeps `lineDefined`: the main function has 0, the handler 1 and its
closures the line they start on. Instructions are 32 bits with the opcode in
bits 0-5, C in 6-14, B in 15-23 and A in 24-31. An RK operand of 250 or more
is constant `RK - 250`.

`Chunk.h` reads this into plain structures and writes it back. All 251 Pro
Street chunks come back byte for byte.

## Chunk format (Lua 5.1, game variant)

Undercover and World build Lua 5.1 the same way: little-endian on every
platform, 4-byte int, size_t and Instruction, and a 4-byte float
`lua_Number`. Stock 5.1 `luac` writes doubles, so it can't read these either.

```
header     1B 'L' 'u' 'a'   signature
           51               version 5.1
           00               format 0, the official one
           01               little-endian
           04 04 04 04      sizeof int, size_t, Instruction, lua_Number
           00               numbers are not integral (a float)
function   string source    u32 size including the NUL; 0 means NULL
           i32  lineDefined, lastLineDefined
           u8   nups, numparams, is_vararg, maxstacksize
           i32 n, n x u32                        code
           i32 n, n x (u8 type, value)           constants: 0 nil, 1 boolean (u8), 3 float, 4 string
           i32 n, n x function                   nested functions
           i32 n, n x i32                        line info
           i32 n, n x (string, i32, i32)         locals: name, startpc, endpc
           i32 n, n x string                     upvalue names
```

Against 5.0, the header has no test number, a function records the line of
its `end` too, code comes before the constants, a constant can be a boolean,
and `is_vararg` is a set of flags: 2 for a function taking `...`, plus 1 and
4 for the old `arg` table that 5.1 keeps with `LUA_COMPAT_VARARG`. 5.1's
`luac -s` writes NULL for every source, and Undercover's chunks were stripped
that way. Each main function takes `...` (every 5.1 main does), has no
parameters and defines the handler, which takes `self, message, vars`.

Instructions have the opcode in bits 0-5, A in 6-13, C in 14-22 and B in
23-31. An RK operand with bit 8 set is constant `RK & 255`. There are 38
opcodes: TESTSET, FORPREP, MOD, LEN and VARARG are new, TFORPREP and
SETLISTO are gone.

World's `lithium.bin` (the 2009 beta) is one chunk whose main function holds
268 functions, each the main function of a former handler chunk. Unlike
Undercover's, it keeps line info and a source name, the handler's hash.

All 214 handlers of each Undercover platform and `lithium.bin` come back byte
for byte.

## The compilers

`external/lua50` holds the core of Lua 5.0.1 (`src/`, from lua.org, with
its `COPYRIGHT`). Each `lua50_*.cpp` compiles one Lua source file as C++
inside `namespace lua50`, so none of Lua's `lua_*`/`luaX_*` symbols can clash
with a second Lua version linked later. `lua50_config.h` includes the C
headers first and sets `lua_Number` to `float`. Numerals are still read with
`strtod` and then rounded to `float`, Lua's default `lua_str2number`.

One line of the sources changed. `llimits.h`'s `IntPoint` casts a pointer
straight to `unsigned int`, which C++ rejects on a 64-bit host. It now goes
through `size_t`, as Lua 5.1 does. The value only seeds a hash bucket, so the
generated code doesn't change.

`lua50_compile.cpp` runs the parser on a source string and copies the
prototypes out. `Compile` writes them in the game format and strips them the
way `luac -s` does. Syntax errors come back as `line N: message` with
`CompileResult::line` set.

### Which 5.0 release

The executables carry no version string, so the round trip has to answer
this. The 5.0.1 compiler reproduces every retail chunk. 5.0.2 changed nothing
in the compiler, so it matches as well. Plain 5.0 and 5.0.3 also reproduce
all of the game data. They differ from 5.0.1 elsewhere: 5.0
sizes the stack for `local function` differently and 5.0.3 patches the
register of a TEST under `not`. My randomly generated test programs show the
difference (130 and 10 of about 2000 compile differently), but no script in
the games uses those constructs. So the data narrows it to 5.0.x and can't
pick a patch level. 5.0.1 is the version `project_description.txt` names, and it fits.

### Lua 5.1.3

`external/lua51` holds the core of Lua 5.1.3 (`src/`, from lua.org's
`lua-5.1.3.tar.gz`, with its `COPYRIGHT`), built the way the 5.0 core is: each
`lua51_*.cpp` compiles one source file as C++ inside `namespace lua51`. The
two cores share no symbol, and no translation unit sees both: callers include
only `lua50_compile.h` or `lua51_compile.h`, which include no Lua header.
None of the 5.1 sources changed. Its `luaconf.h` sets `lua_Number` to double
with no way to override it, so `lua51_config.h` includes it first and then
replaces the number settings with float's. 5.1's `IntPoint` already goes
through `size_t`.

Undercover's PC executable, the 360 debug build and World's `nfsgame.dll`
all contain `$Lua: Lua 5.1.3`, and the data agrees. Compiling each
`function h_` piece of World's `lithium.lua` as its own chunk gives the
matching function of `lithium.bin` in every case but for some numbers. 260
of the 268 match outright. The other 8 hold numerals between 0 and 1, which
`lithium.bin` stores one unit in the last place above what `strtod` reads:
0.5 is 0.50000006, 0.1 is 0.100000009, 0.25 is 0.25000003. Numerals of 1 and
more (1.5, 2.7, 5.5) come out right. Undercover's chunks show the same
thing, with 0.50000006 as their only fractional constant. So the authoring
tool parsed numbers its own way. `Compile` reads numerals
with `strtod` as stock Lua does, and the decompiler prints those constants as
they are stored (`0.50000006`), which compiles back to the same bits.

`Compile(source, Version::Lua51)` is stock 5.1.3 with `LUA_COMPAT_VARARG`,
the default, so a function with `...` also gets the `arg` local. Errors come
back as for 5.0.

## The decompilers

`Decompile50.cpp` reads the bytecode as the output of that exact code
generator, statement by statement. In outline:

- **Registers.** Lua 5.0 keeps locals in the lowest registers and evaluates
  expressions in a stack of temporaries above them. The decoder tracks the
  pending temporaries and builds expressions as instructions consume them.
  A value that is read out of order, read twice, read again later
  (register liveness) or never read is a local, and becomes a `local`
  statement where it was computed. Multi-result calls become
  `local a, b = f()`.
- **Statements and loops.** Jump shapes identify the loops before decoding.
  A numeric `for` is the `SUB`/`JMP` prologue plus `FORLOOP`. A generic `for`
  is `TFORPREP` and `TFORLOOP`. A `while` starts with a jump to its condition,
  which the compiler moves after the body. A `repeat` is a jump back from its
  `until` condition. `if`/`elseif`/`else` come from conditional jumps and the
  jump that skips the `else`. Jumps that the compiler chained through a
  following jump (`break`, an `else` jump) are followed back to where they
  logically point.
- **Conditions.** A run of tests is rebuilt into `and`/`or`/`not` by finding
  where each jump lands: the true exit, the false exit, or the start of the
  next operand. The same rebuild handles `and`/`or` used as values, which end
  with the value in one register or with a pair of `LOADBOOL`s.
- **The rest.** Multiple assignment (stores run last target first, values in
  registers), table constructors (the `NEWTABLE` size hints tell named fields
  from later `t.x = v` assignments), closures and their upvalues,
  `local function` (the closure captures its own register), method calls,
  varargs (`arg`), `do ... end` blocks (from `CLOSE`, or from a register being
  reused after its local's scope ended).
- **Exact output.** Constants enter the constant table in source order, so
  printing has to keep that order. That matters for `a > b`, which compiles
  to `b < a`. The printer picks the side whose constants come first. Literals
  the compiler stored but folded away, such as `while 1 do`, are printed back
  so the table matches. Each nested function goes on the source line its
  header records. Blank lines pad the source when the original had comments
  or spacing, and lines are joined when it was denser.
- **Checking.** `Decompile` compiles its own output and compares it with the
  chunk, ignoring debug information. `DecompileResult::exact` reports the
  result, and a warning names the first differing function and instruction.
  When a guess about a register turns out wrong later, the decoder retries
  with that guess flipped. A function it still can't read keeps its header and
  gets its bytecode listing as comments, so the rest of the chunk still
  decompiles.

Locals and parameters get names from what they hold (`local racerInfo =
RaceStatus:GetRacerInfo(...)`), or `local1`, `p1` and so on when nothing fits.
A handler's parameters are always `self, message, vars`. Names never shadow a
global the chunk uses or a visible local.

### What it can't do

- Names, comments and formatting are gone from the chunks. Only the line each
  nested function starts on survives.
- Forms that compile the same come out one way. `not (a == b)` prints as
  `a ~= b`. `if a then if b then ... end end` may print as `if a and b then`.
  `x = x or {}` and `if not x then x = {} end` are the same bytes. A
  `do ... end` without captured locals leaves no trace unless the registers
  show it.
- Comparisons may come out mirrored (`5 <= x` for `x >= 5`) when the constant
  order doesn't decide.
- Folded constants are restored only where the order is clear: the condition
  of `while`, otherwise as `if <constant> then end`, which compiles to no
  code.
- Strange code can defeat it. It was test on randomly generated programs that
  apply operators to tables and nil and nest deeply. About 91% of small ones
  and 73% of large ones come back exact. The rest compile but differ, or keep
  a function as a commented listing. The games' scripts don't hit these cases.

### Lua 5.1

`Decompile51.cpp` is a port of `Decompile50.cpp` with the same design and
the same check, adapted where 5.1.3's code generator differs.

- **Tests.** 5.1 splits 5.0's TEST in two. TEST A C tests R(A); TESTSET A B C
  tests R(B) and copies it to R(A) when it jumps. `patchtestreg` turns every
  TESTSET of a statement's condition into a TEST, so a TESTSET always belongs
  to a value (`x = a or b`). The decoder reads TEST as 5.0's TEST A A C and
  TESTSET as TEST A B C, and the condition rebuilding is unchanged.
- **Loops.** A `while` keeps its condition in front of the body and ends with
  a JMP back to it. A `repeat` ends with the conditional jumps of its `until`,
  whose condition sees the body's locals. Every jump back marks a loop start,
  and the last one belongs to the outermost loop. When several loops start at
  one instruction (`repeat while a do ... end until b`), the body is read
  first as statements, then again trying the inner loops first. `while true
  do` and `repeat ... until false` compile the same, and come out as `while
  true do`. A numeric `for` is FORPREP and FORLOOP, with the loop variable
  after the three control registers. A generic `for` is a JMP to TFORLOOP,
  which jumps back.
- **Constants.** 5.1 folds numerals at compile time: `-5`, `2 * 3` and `-0.5`
  are one constant each, and a numeral only enters the constant table where an
  instruction uses it. A numeral on the left of arithmetic waits for folding,
  so `codearith` adds the right operand's constants first, and the printer's
  constant order follows that. nil, true and false can be RK constants (`x ==
  nil`, `vars.done = false`). Only strings can sit in the table with nothing
  referring to them (`not "x"` folds to false). They come back as `if "x" then
  end` between statements, or as `not "x"` and `"x" and v` inside one.
- **Tables.** NEWTABLE's sizes are `luaO_int2fb` counts, exact below 16, and
  every field counts toward the hash size, `[k] = v` as well as `name = v`.
  SETLIST stores 50 items per block.
- **Locals without code.** `luaK_nil` emits nothing for nils that land in free
  registers before a function's first instruction, so `local a, b` at the top
  of a function leaves no trace. Such a function starts with nils pending in
  the registers read before anything writes them, and in registers that a
  later instruction finds occupied though nothing wrote them.
- **Extra values.** `local a = x, y` evaluates y into the register after a and
  never reads it. When an instruction writes over such dead pending values,
  they join the declaration below them. With no value below, they were locals
  of a `do ... end` that ended without a CLOSE. 5.0's decoder doesn't do this.
- **Lines.** A 5.1 function records the line of its `end` as well as its
  header, and the check compares both. The printer places both: blank lines go
  before an `end` that would come too early, and lines are joined when the
  body runs long. Undercover's handlers kept their authors' blank lines and
  comments, so a decompiled body is often followed by a run of blank lines.
- **New syntax.** `%`, `#` and `...` (VARARG). A statement that starts with
  `(` gets a `;` on the statement before it, which 5.1 otherwise rejects as
  ambiguous.

What it can't do, beyond 5.0's list:

- A `repeat` whose body locals are captured by a closure compiles to another
  shape, with a CLOSE and a `break` before the jump back. The decoder doesn't
  read it.
- `not (x and 3)` folds the numeral away. The decoder doesn't guess one.
- Random programs from my generator (operators on tables, functions and nil,
  nested closures, every statement and loop kind) come back exact in 1333 of
  1500 small cases (about five statements) and 292 of 500 larger ones. The
  generator is harsher than the one that was used for 5.0: on the same programs
  written without `#`, `%` and `...`, the 5.0 decompiler gets 859 of 1500 and
  88 of 500 where the 5.1 one gets 1359 and 283. The games' 5.1 chunks are
  all exact.

## Round-trip results

| Data                           | Version | Read and write  | Decompile then compile               |
| ------------------------------ | ------- | --------------- | ------------------------------------ |
| Pro Street PC (`gameplay.lzc`) | 5.0     | 251 / 251       | 251 / 251                            |
| Most Wanted PC                 | 5.0     | 258 / 258       | 258 / 258                            |
| Carbon PC                      | 5.0     | 312 / 312       | 312 / 312                            |
| Undercover Wii                 | 5.0     | 455 / 455       | 455 / 455                            |
| Undercover 360                 | 5.1     | 214 / 214       | 214 / 214                            |
| Undercover PC                  | 5.1     | 214 / 214       | 214 / 214                            |
| World 2009 beta `lithium.bin`  | 5.1     | the whole chunk | 268 / 268 functions, each as a chunk |

`tests/test_lua.cpp` checks all of these through `Database` (`attribtests lua:`),
and the compiler oracle on `lithium.lua`. A one-off scan of every Lua 5.0
database in `attrib/databases` (360, PS2, GameCube, Wii, betas and demos),
which looked for LZHeader blocks in the files, found 969 distinct 5.0 chunks,
all exact.
