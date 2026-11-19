# Scripts and object properties (partly understood)

**Property sets.** Objects carry lists of property assignments that the game binds at load time. An
object's word at +0x28 points to a pair whose second word points to a record; the record's first word
points to the list. Each entry is a set-instruction with its value inline: `0x23 00 tttt` followed by three
link words and the value, or `0x2A 00 tttt` followed by four words and the value (`tttt` is the value type;
16-byte values are aligned to 16). The list ends at the first word that is neither. The `SPLP` chunk
names, for each entry, the class and the property it sets. `ina_tool props` lists them;
`readObjectProperties` reads them.

**Fills** (class 0x212) are timed full-screen effects that scripts switch on and off, described entirely
by their property sets: `FADE_IN_TIME`, `PEAK_TIME` or `HOLD_AT_PEAK`, `FADE_OUT_TIME`, and what they do
at full strength: `OVERLAY_COLOR` and `OVERLAY_OPACITY`, `ADD_BLOOM` and `ADD_BLOOM_COLOR`,
`MULTIPLY_BLOOM`, fog, depth of field, desaturation, sound settings. Colours are 0..255. Scripts call
`trigger`, `UNTRIGGER` and `STOP(seconds)`. The engine implements the envelope and the overlay, and
approximates added bloom by an overlay of the bloom colour; the reading of the envelope (fade in, hold or
stay for the peak time, fade out; `UNTRIGGER` fades out over the fill's own time, `STOP` over the given
time) follows the property names and is not yet checked against the game.

**The script machine (located, being decoded).** Property sets are programs for a bytecode machine, and
so is all mission logic. Instructions are 32-bit words with the opcode in the top byte; some are followed
by operand words (a constant, a field offset, or the three words that `SPLP` links to a class and a
property). The game's interpreter has been found in the executable: it reads the opcode byte at the
program counter and dispatches through a table of 185 entries (0x00..0xB8, 133 distinct handlers, four of
them invalid). Seen so far, provisional until every handler has been read: constant loads (0x01 object
id, 0x02 boolean, 0x03..0x0A typed), field loads (0x1C), property set and get by type (0x23..0x2D), method
calls by name (0x37, 0x3E), branches (0x43..0x45), native calls (0x88) and the list end (0xB1).

What has been read so far (from the interpreter and checked against the opening mission's data):

- It is a typed stack machine with nested scopes. Instructions reserve and release stack bytes, push
  constants (inline, any size), load and store scope variables by (scope depth, offset), get and set
  engine properties, branch, call script functions and call engine functions.
- A script function is a 16-byte closure value: a code pointer plus the scope it was created in. Functions
  start with an entry instruction carrying their frame size and end with a return.
- **Script objects.** A script object is an initialisation block that stores constants and closures into
  its scope variables, followed by a symbol table of 16-byte entries: name, offset, type, zero. The table
  names every member variable and method, so every function can be named. The opening mission has 319
  functions (`OnMissionStart`, `Setup`, `PlayDialog`, `RunBattle`, per-sector `SetupHex...`).
- **Links to the engine.** `SPLP` has three sections, counted by its first three words: property links of
  3 words (offset, class, property), property links of 7 words, and function links of 6 words (the offset
  of a call instruction, a slot, the function's name) with extra words after some. The opening mission
  links 716 calls to 81 engine functions; the most used are `Sleep`, `CtsThreadCreate` (start a script
  thread), `RetractThread`, `ObjFind`, `HeroFind`, `MissionTriggered`, `SndEffect`, `EventCreate`,
  `WaitForEvent`. Scripts are cooperative threads that sleep and wait.
- **Clip events.** Clip objects can carry an event track (a fraction of the clip's length and a name);
  the opening shot's clip names six moments of the blast. Script objects receive them as labels.

## The script machine (`engine/script/`)

The engine now has its own machine for this byte code. `ina_tool script <archive> <pack> <oid_manifest.txt>
<function> [seconds]` runs one script function with no engine behind it and prints, in time order, what the
script asks for; run on the opening mission's entry function it reaches the end of the function. What the
machine implements is what was read from the game's interpreter and then confirmed by that run.

**Values and the stack.** One byte stack per thread, growing upward; values are big-endian. Type codes: 1
bool (1 byte), 2 int, 3 float, 7 name (4 bytes each), 4 and 5 vectors (16 bytes, 16-aligned), 6 matrix (64
bytes), 14 function value (16 bytes: code, 0, scope, 0), 16 (8 bytes); codes with bit 15 set are lists and
codes with bits 12-13 set are object references (4 bytes). A list is a record (data pointer, count, element
type). Pushing a constant first aligns the stack to the constant's alignment.

**Instructions** are 32-bit words: opcode and three operand bytes, some followed by extra words.

| Opcode | Extra words | Meaning |
|---|---|---|
| 01 | data | push `b1` inline bytes with alignment `b2` |
| 02 | | push the bool `b1` |
| 03-0A | 1 (offset) | copy an argument (size `b1`, `b2` bytes below the caller's stack mark) into the scope |
| 0B | 1 + data | copy `b1` inline bytes into the current scope |
| 0C-1B | 1 (offset) | store the top `b1` bytes into the scope `b2` levels up, then pop `b1 + b3` |
| 1C | 1 (offset) | load `b1` bytes from the scope `b2` levels up into space reserved on the stack |
| 1D | 2 | function entry: new scope of `b2b3` bytes under the current one, new stack frame |
| 1E / 1F | 1 / 0 | open / close a nested scope |
| 22 | | value bookkeeping (reference counts in the original); pops `b1` |
| 23, 26, 2A, 2C / 24, 25, 2B, 2D | 3 (2x: 4) | set / get a property: object pointer and value sit in the top `b1` bytes |
| 34-36 / 38, 39 | 1 (+1 zero) | engine method / engine function: result slot and arguments are the top `b1` bytes |
| 37 | 2 (name, 0) | call a script method by name on the object at the top |
| 3A / 3B / 3D | | pop / reserve / zero `b1` bytes |
| 3C | | slide `b1` bytes down over `b2` bytes |
| 3E | | call the function value at the top (pops `b1`) |
| 3F | | bind the current scope into the function value at the top |
| 40 | | return; `b2` result bytes go to the slot `b1` below the caller's mark |
| 41 | | branch if a bool argument is set |
| 42 | | jump |
| 43 / 44 / 45 | | branch if the top bool is false (43 always pops; 44 and 45 keep it when taken; 45 tests true) |
| 46 / 47 / 48 | | list loop: test index against count / fetch the element into a scope variable / next |
| 4B-6B | | arithmetic, in place: operands in the two slots below the top, result in the slot below them |
| 6C-7F | | comparisons, same layout, 1-byte result |
| 80-85 | | and, or, max, min |
| 88, 8D-90, 9D | | not; int to float, float to int, int to bool, bool to int, is zero |
| 91, 95, 96, 98 | | list element, build list, list count, list contains |
| A6 / A7 / A8-AF | 2 / 0 / 0 | make a structure from the top `b1` bytes / load a field / store a field |
| B0 | | push global object number `b2b3` (the pack's table of object names; `b1` = 1 for object lists) |
| B1 | | end of an initialisation block |
| B3, B4 | 2 / 4 | zero-initialise a value |
| 20, 9F, A0, A1-A3, A5, B5-B8 | 1-4 | debug records, skipped |

Branch targets are relative to the instruction. Script time is counted in ticks, 1440 a second; opcodes
4D, 53 and 54 mix tick counts with seconds.

**Calls.** The caller reserves the result slot, pushes the arguments, pushes the function value and calls;
the callee copies its arguments into its scope; on return the caller pops the arguments. Engine calls
write their result over the first of their bytes.

**Pack tables.** Offset 0x40 of the pack's memory points to the table of global object names and 0x44
holds its length. A script class is a (initialisation block, symbol table, symbol count) triple inside its
owning object; running the initialisation block in a fresh scope binds the class's functions and sets its
member variables.

**Not implemented yet:** list concatenation and search (97, 99), random element
(9C), string building (9E), the property forms 27-33,
matrix arithmetic. Object lists are returned empty. Scopes are never freed. Engine
functions other than threads, sleeping and time are not implemented: the opening needs fades, the title
prompt, the cameras, sound, time of day, spawning and the rest (see docs/PROGRESS.md).

**The opening sequence**, as its script function reads: a screen fill; time-of-day adjustment; after a
few seconds the logo-and-prompt object is triggered; the script polls the pad each frame until the start
button is pressed; the prompt is removed, the explosion's music list and a sound mix start, and the first
camera plays its `start` clip (the blast, with its six labelled moments); the district's power grid is
switched off; a second camera shot follows with another sound mix; fills cover the cut; the mission's
first subtask is completed and a thread warps the player on. Not yet resolved: the script functions called
between those steps, and what each label does.

**Missions.** Each mission has its own `.sprig.xpp` pack (140 of them). They hold the mission's objects:
transform markers (`XoData`), trigger and camera volumes, logic lists (`LoListData`), user-interface
objects and the mission's sound banks, and one `MissionData` object. `EMPIRE.xpp` holds the city-wide
task graph (`TaskGraphData`) and mission start points.

Not decoded: most of the instruction set, how scripts attach to objects and events, the task graph, and
how missions are sequenced.
