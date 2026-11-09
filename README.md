# uudumber — test target, scanner and dumper for UE4 on Android

[kp7742/UE4Dumper](https://github.com/kp7742/UE4Dumper) is a *walker*: you hand
it GNames, GUObject and GWorld and it iterates them. It has no scanner, its
offset profiles are unmaintained, and there is no way to tell a wrong offset
from a wrong target because no target has a known answer.

This fills both gaps.

| | |
|---|---|
| `fixture/ue4_fake.cpp` | A synthetic UE4 process — real FNamePool, FUObjectArray and UWorld chain at stock UE 4.25/4.27 arm64 offsets. Prints its own ground truth. |
| `scanner/ue4scan.cpp` | Finds GNames, GUObjectArray and GWorld from a pid alone. |
| `dumper/ue4dump.cpp` | Emits `Logs.txt`, `Names.txt`, `Objects.txt` and a C++ SDK. Walks both property layouts: `UProperty` on `Children` (<= 4.24) and `FProperty` on `ChildProperties` (4.25+/UE5). |
| `common/ue4core.h` | Target access and structure discovery. |
| `common/ue4emit.h` | Dump emission, usable against a local or remote target. |
| `tools/verify.py` | Diffs a dump against the fixture's ground truth. Non-zero exit on failure. |

Because the fixture knows its own answer, a disagreement is a bug in the tool,
not in the target. That has already paid for itself twice — see Findings 1
and 10, both of which were silent failures that looked like success.

## Build and run

```sh
./build.sh      # out/{ue4_fake_host,ue4_fake_arm64,ue4scan_arm64,ue4dump_arm64}
```

Device-free regression test — the fixture dumps itself and the verifier checks it:

```sh
./out/ue4_fake_host --json out/truth.json --selfdump out/selfdump
python3 tools/verify.py out/truth.json out/selfdump
```

On device (no root: both processes run as `shell`, and `process_vm_readv`
between same-uid processes is permitted — this device reports no yama
`ptrace_scope`):

```sh
adb push out/ue4_fake_arm64 /data/local/tmp/ue4_fake
adb push out/ue4scan_arm64  /data/local/tmp/ue4scan
adb push out/ue4dump_arm64  /data/local/tmp/ue4dump
adb shell chmod 755 /data/local/tmp/ue4_{fake,scan,dump} 2>/dev/null

adb shell '/data/local/tmp/ue4_fake --hold 120 --json /data/local/tmp/truth.json &
           sleep 2
           PID=$(ps -A -o PID,NAME | grep -w ue4_fake | tr -s " " | cut -d" " -f1)
           /data/local/tmp/ue4scan $PID --module ue4_fake --verbose
           /data/local/tmp/ue4dump $PID --module ue4_fake --out /data/local/tmp/dump'
```

Fixture flags: `--selftest`, `--json <f>`, `--selfdump <dir>`, `--hold <s>`,
`--filler <n>` (names, default 8000 — enough to cross a 128 KiB block),
`--big` (70000 extra objects, to cross a 65536-object chunk).

## Verified on a real UE 4.27 game

Target: **BattleRush** (`com.bello.battlerush`), a packaged UE 4.27 Shipping
build, arm64, 261 MB `libUE4.so`, running on a Redmi 2312DRAABI (Android 15,
rooted). Scanner given nothing but a pid and the module name:

```
  [pool]  Blocks[0] slot at 0x726acec440 -> block 0x7270810000, blocks=16 cursor=0xff62
  [objs]  TUObjectArray 0x726ad34ee8, NumElements@+0x14 = 15638 itemSize=0x20 chunk=65536
          (sampled 64: live 64, named 64, index-match 64)
  [world] candidate 0x7216d5a0c0 "ThirdPersonExampleMap" persistentLevel=yes

  GNames        +0xfa82430   name[0]="None"
  GUObjectArray +0xfacaed8   objects=15638  obj[0]="/Script/CoreUObject"
  GWorld        +0xfc6bbd0   -> UWorld "ThirdPersonExampleMap"
```

`obj[0] == "/Script/CoreUObject"` is the correct answer for any UE4 build, and
the located world is the actually-loaded level rather than a class default
object. Runtime after restricting the search to the module's own writable
memory: **5.4 s**, down from 2m47s scanning all 1866 writable regions.

Four defects had to be fixed to get there, none of which the fixture could have
revealed — see Findings 12–15.

## Verified on the fixture

Measured on a Redmi 2312DRAABI, Android 15, arm64. Scanner given nothing but a pid:

```
  [pool]  Blocks[0] slot at 0x61b18ca0f0 -> block 0x7a4a3e8000, blocks=2 cursor=0x17354
  [objs]  TUObjectArray 0x61b18da100, NumElements@+0x14 = 106
  [world] UWorld at 0x7956cb08a0 ("TestLevel_Main")

  GNames        +0x620e0   name[0]="None"        truth +0x620e0  OK
  GUObjectArray +0x720f0   objects=106           truth +0x720f0  OK
  GWorld        +0x72120   -> "TestLevel_Main"   truth +0x72120  OK
```

Dump of that process, verified against the device's own ground truth — 14/14 checks:

```
Names.txt    8110 ids all map to the right name; 3396 of them in block >= 1
             one 600-char name survives (a signed header read would not)
Objects.txt  106 objects; Engine.Actor, Engine.Character, MyGame.BP_Hero_C,
             MyGame.LateBlockClass all present
SDK          ACharacter : APawn, ABP_Hero_C : ACharacter,
             Health at 0x0400, InventorySlots[4], Loadout[3]
```

Scale test, `--big`: **70,106 objects across 2 chunks, 78,110 names across 14
blocks**, all three globals correct, full dump in 5.06s.

Sample SDK output:

```cpp
// Class Engine.Character
// Size: 0x0438 (Inherited: 0x0310)
class ACharacter : public APawn
{
public:
    uint8_t                      Pad_0[0xF0];                 // 0x0310(0x00F0) MISSED
    float                        Health;                      // 0x0400(0x0004)
    float                        MaxHealth;                   // 0x0404(0x0004)
    uint8_t                      TeamId;                      // 0x0408(0x0001)
    uint8_t                      Pad_1[0x7];                  // 0x0409(0x0007) MISSED
    class UObject*               Mesh;                        // 0x0410(0x0008)
    class UObject*               InventorySlots[4];           // 0x0418(0x0020)
};
```

## How discovery works

No offsets are supplied. Each global is found from an invariant the engine
cannot break without breaking itself.

- **GNames** — scan writable memory for a pointer whose target decodes as FName
  id 0: header `len == 4`, `!wide`, bytes `"None"`. That slot is `Blocks[0]`.
  Corroborated by `CurrentBlock`/`CurrentByteCursor` being in range and a
  second name decoding.
- **GUObjectArray** — scan for a pointer to a chunk list, then validate with
  **`UObject::InternalIndex == its own array index`**, sampled across the array
  (first 8, four interior points, the last). `NumElements` is probed across
  +0x08..+0x20 and the candidate with the largest valid count wins.
- **GWorld** — walk the array for the instance whose class is `World`, then
  scan writable memory for an 8-byte value equal to its address.

## Findings

### 1. Android tags heap pointers; range checks must untag

Measured on device:

```
Blocks[0] stored   = 0xb400007e4a13e000
Blocks[0] untagged = 0x7e4a13e000
read via TAGGED    -> 8 (ok)
read via UNTAGGED  -> 8 (ok)  first bytes: 00 01 'None'
```

Both *dereference* fine — the kernel ignores the top byte — which is why
upstream's `getPtr` (`Mem.h:160`, a raw read, no masking) works without ever
untagging. What does not work is comparing a stored pointer against
`/proc/pid/maps`: `0xb400007e4a13e000` is in no mapping. Any scanner must mask
to 56 bits. This cost the first scanner run a total miss — every candidate was
rejected before it was ever read.

### 2. `TUObjectArrayToNumElements` is a footgun, not a bug — `Offsets.h:194`, `:250`

*Corrected.* An earlier version of this file called the `0xC` in
`initOffsets_64()` simply wrong. It isn't: `patchUE423_64()` overrides it to
`0x14` when `--newue` is passed, and `0x14` is right for 4.23+ — the same value
our probe measures.

The hazard is that the two paths disagree and the flag is manual. Run a 4.23+
target without `--newue` and you read `0xC`, which lands on `MaxElements` or
padding — and Finding 3 then hides the consequence instead of reporting it.
Our scanner probes rather than depending on a flag.

### 3. A wrong object count is silently replaced with 300000 — `GUObjects.h:55-56`

```c
if (ocount < 10 || ocount > 999999) { ocount = 300000; }
```

With a wrong count offset the dump doesn't fail — it iterates 300k garbage
indices through `UObject::isValid` (`StructsSDK.h:86`), which only checks that a
name id and class pointer are nonzero. No bounds check, no mapping check.
Combined with Finding 2, this is the most likely cause of a run that produces
nothing.

### 4. FName ids are wrong after the first entry in a block — `FNames.h:206`

```c
Offset += StrLength / Offsets::FNameStride;                       // 206
uint16 bytes = Offsets::FNameEntryToString + StrLength * (...);   // 207
It += (bytes + Offsets::FNameStride - 1u) & ~(Offsets::FNameStride - 1u);
```

`It` advances by the aligned entry size (header + chars); `Offset` advances by
`StrLength / stride`, omitting the 2-byte header. `"None"` occupies 6 bytes = 3
stride units but `Offset` moves 2, and the error compounds across the block, so
the `[key]` in `Strings.txt` is not the FName id.

The fixture proves it: `"Class"` is genuinely id 3; upstream's arithmetic emits 2.
Our `forEachName` advances by the whole aligned entry, and the verifier confirms
all 8110 ids.

### 5. Wide entries advance by `sizeof(wchar_t)` — `FNames.h:208`

4 on Android. UE stores wide names as UTF-16, 2 bytes per char — and
`WideStr::w_str` reads them correctly as `UTF16`. So the decode is right and the
advance is wrong, desynchronising everything after the first wide name. The
fixture contains two wide names to catch this.

### 6. Signed header read truncates long names to "None" — `FNames.h:77`, `:172`

```c
auto FNameEntryHeader = Read<int16>(FNameEntry);
int StrLength = FNameEntryHeader >> Offsets::FNameEntryToLenBit;
```

Arithmetic shift on a signed value: any name of length >= 512 sets the sign bit,
yields a negative length and falls through to `return "None"`. The fixture holds
a 600-char name; the verifier asserts it survives.

### 7. `getArrayDim` reads `ElementSize` — `StructsSDK.h:170`

```c
static int32 getArrayDim(kaddr prop) { return Read<int32>(prop + 0x34); }
```

`0x34` is `UPropertyToElementSize` on 64-bit; `ArrayDim` is `0x30`. Every array
dimension in a generated SDK is actually the element size. The fixture has
`InventorySlots` at `ArrayDim 4, ElementSize 8` — upstream renders `[8]`.

### 8. Surrogate pair check compares an element with itself — `FNames.h:36`

```c
if (is_high_surrogate(uc) && is_low_surrogate(source[i]))
```

`uc` *is* `source[i]`, so this asks whether one code unit is both a high and a
low surrogate — never true. Should be `source[i + 1]`.

### 9. `GNameLimit = 170000` hardcoded — `FNames.h:7`

The pre-4.23 path stops there regardless of the real table size.

### 12. `FUObjectItemSize = 0x18` is wrong on a real 4.27 build — `Offsets.h:186`

**Measured, on BattleRush: `0x20`.**

Stock `FUObjectItem` is `UObjectBase* Object; int32 Flags; int32
ClusterRootIndex; int32 SerialNumber` — 20 bytes padded to 24 (`0x18`), which
is what upstream hardcodes. A build that compiles in the GC-tracking or stat
fields makes it 32 (`0x20`), and this packaged 4.27 game does.

Unlike Finding 2 there is no flag to save you: `FUObjectItemSize` is set only in
`initOffsets_64()` and is never touched by `patchUE423_64()` or
`patchCustom_64()`. A wrong item size makes every index resolve to the wrong
address, so the array reads as garbage — and Finding 3 then replaces the count
with 300000 and grinds through it.

Our scanner probes `{0x18, 0x20, 0x10, 0x28}` and keeps whichever one makes
`InternalIndex == index` hold.

Chunk size cannot be found the same way: with only 15,638 objects every index
lands in chunk 0, so 8192/16384/32768/65536 are indistinguishable by sampling.
It comes from `MaxElements / MaxChunks` = 196608 / 3 = **65536** instead.

### 13. (ours) A real object array has holes

The first real-game run rejected 1.8 M candidates and found nothing, the true
array among them. Cause: `plausibleArray` required *every* sampled slot to
resolve. In a shipped build destroyed objects leave null slots and GC empties
whole ranges, so that rule rejects the genuine array.

Fixed by scoring rather than gating — holes are skipped, and a candidate is
judged on whether the objects that *do* resolve report back the index they were
looked up at. On the true array that is 64/64; the nearest false candidate
managed 1/7.

### 14. (ours) `Default__World` is a decoy

Taking the first object whose class is `World` finds UWorld's class default
object, not the live world. It has the right class and resolves cleanly, but
nothing points at it, so the follow-up pointer scan comes back empty and GWorld
reports NOT FOUND while the scanner believes it found a world.

Fixed by skipping names beginning with `Default__` and preferring a world whose
`PersistentLevel` is mapped.

### 15. (ours) Android names its anonymous mappings

`.bss` appears in `/proc/pid/maps` as `[anon:.bss]`, not as an empty path:

```
726aba1000-726abc7000 rw-p  .../libUE4.so     <- .data
726abc7000-726afbe000 rw-p  [anon:.bss]       <- 4 MB, holds the globals
```

Treating "anonymous" as "empty pathname" when walking contiguous regions
excluded the one region that matters, and the scan found nothing in 0.15 s —
a fast, confident, wrong answer.

### 11. `UStructToChildProperties = 0x44` cannot be right — `Offsets.h:252`

`patchUE423_64()` sets a **64-bit pointer field at offset 0x44**, which is not
8-byte aligned. In stock UE 4.25+ `UStruct` is `SuperStruct 0x30`,
`Children 0x38`, `ChildProperties 0x40`, `PropertiesSize 0x48`. `0x44` reads
four bytes into `ChildProperties` and four bytes of whatever follows, so the
FField chain starts from a spliced value.

Flagged as suspected, **not verified** — confirming it needs a real 4.25+
binary, which this repo does not yet have.

### 10. (ours) A lone aliased pointer passes a weak array check

First device run of our own scanner reported `GUObjectArray +0x620b8` against a
truth of `+0x720f0`, with `NumElements == 1`, and produced a dump of exactly one
object — no error, no warning. The candidate was a slot *inside the name pool's
own `Blocks[]`*, which is a long run of heap pointers and therefore the richest
source of false positives in the process.

Validating "item 0 resolves, and the last item resolves" is not enough when the
count is 1 — both are the same object. The fix was to require
`UObject::InternalIndex == index` across a sample of the array, demand
`n >= 4`, exclude the pool's address range, and keep the highest-scoring
candidate instead of the first. Worth noting for anyone writing their own
scanner: the failure mode is a clean-looking dump, not a crash.

## Status

- Fixture, scanner, dumper, verifier: built and run on-device (Redmi
  2312DRAABI, Android 15, arm64). Every result above is real output.
- Findings 1, 2 and 10 are measured. 3–9 are from reading upstream source;
  4, 6 and 7 are additionally asserted by `tools/verify.py` against the fixture.
- **Validated against a real packaged UE 4.27 game** (BattleRush, arm64
  Shipping). All three globals located from a pid alone; see the section above.
  The fixture turned out to be necessary but not sufficient: four real defects
  (Findings 12–15) only appeared against a shipped engine build, and three of
  them produced a confident wrong answer rather than an error.
- Not yet done on the real target: a full SDK dump. The scan is verified; the
  `ue4dump` run against BattleRush was interrupted when the device was
  unplugged.
- Not done: patching upstream, the ELF dump/rebuild path, pre-4.23
  `TNameEntryArray`, 32-bit, wide-name output as real UTF-8.

## Getting a real engine build onto this machine

Checked 2026-11-07. Blockers, in order:

| | |
|---|---|
| Epic Games Launcher | not installed — needs your Epic account, GUI login |
| JDK | only 17 present. UE4.27 Android wants 8 or 11; UE5 wants 17 |
| NDK | 26.1 and 27.1 present. UE4.27 pins **r21e** |
| CPU | Apple M2. UE 4.27 is x86-only, runs under Rosetta; UE5 is native |
| Disk | 119 GB free — sufficient |

**UE5 is the practical target here**, despite the repo being named for UE4: it
runs natively on Apple Silicon, uses the JDK already installed, and shares the
FNamePool and FUObjectArray this code targets. The one thing that differs is
the property layout — which is exactly why FField support was added before
attempting the install rather than after.

Steps that need you: install the launcher, sign in, install UE5, create a blank
**C++** project, enable Android in Project Settings, package Shipping for
arm64. From the packaged APK onward this repo takes over.
