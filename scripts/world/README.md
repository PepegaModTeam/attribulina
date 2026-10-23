# World's types

No header of any World build is known, so `scripts/gen_schema.py --game world` builds `data/schema/world_pc.txt` from Carbon's headers (World began as Carbon), then the two headers here, read before Carbon's, then Undercover's debug header for what Carbon lacks. One schema covers the final build and both betas: the betas' type tables add the types the final build lacks.

```bash
python3 scripts/world/easharp.py                            # writes easharp_types.h (about a minute)
python3 scripts/gen_schema.py --game world --platform pc    # writes data/schema/world_pc.txt
python3 scripts/nfsms/gen_words.py --game W --game WB       # the nfsms word tables after a schema change
```

- `easharp_types.h` is generated. The 2010 beta's EASharp bindings expose World's `GameCore` code to C#. `exes/pc/world-2010-beta/gameplay.dll` (the managed side) has the enums, with their values, and classes whose properties wrap native members. That build's `nfsw.exe` registers each native accessor by name, and a setter's store gives its member's offset and width. `clrmeta.py` reads the assembly's metadata tables and finds the registration records.
- `world_types.h` is written by hand: the types only NFS-VltEd's World classes, the final build's data and its `nfsw.exe` describe. Each says where its layout and names come from. A member nothing names is `Value<n>`, after NFS-VltEd's property number.

The bindings are the 2010 beta's. The final build's type table has the same sizes for the types they share, and its data reads sensibly with them (`gameplay.BlackBoardMap`: `{ kBlackBoard_AI, ai_event, 3 }`). A value the final build added to an enum shows as a number.
