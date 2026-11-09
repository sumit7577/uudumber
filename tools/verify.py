#!/usr/bin/env python3
"""Diff a dump produced by ue4dump against the fixture's ground truth.

    python3 tools/verify.py <truth.json> <dump-dir>

Exit status is non-zero if any check fails, so this works in CI.
"""
import json
import re
import sys
from pathlib import Path

def main(truth_path, dump_dir):
    truth = json.loads(Path(truth_path).read_text())
    dump = Path(dump_dir)

    expect = truth["expect"]
    truth_names = {int(n["id"]): n["name"] for n in truth["names"]}

    failures = []
    def check(label, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {label}{(' — ' + detail) if detail else ''}")
        if not ok:
            failures.append(label)

    # ---- Names.txt --------------------------------------------------------
    print("Names.txt")
    names_file = dump / "Names.txt"
    if not names_file.exists():
        check("file exists", False)
    else:
        got = {}
        for line in names_file.read_text(errors="replace").splitlines():
            m = re.match(r"\[0x([0-9a-f]+)\]\s(.*)$", line)
            if m:
                got[int(m.group(1), 16)] = m.group(2)

        check("name id 0 is 'None'", got.get(0) == "None", repr(got.get(0)))

        # Every truth name must appear at its exact id. This is the check that
        # catches the upstream offset-drift bug: names decode, but under ids
        # that are wrong.
        mismatched = [(i, truth_names[i], got.get(i)) for i in truth_names
                      if got.get(i) != truth_names[i]]
        # wide names are stored UTF-16; the dumper renders them back to ASCII
        mismatched = [m for m in mismatched if m[2] is not None or True]
        check(f"all {len(truth_names)} ids map to the right name",
              not mismatched,
              f"{len(mismatched)} wrong, first: {mismatched[0] if mismatched else ''}")

        # Block-1 names only resolve if block indexing is right.
        late = [i for i in truth_names if i >> 16 >= 1]
        late_ok = all(got.get(i) == truth_names[i] for i in late)
        check(f"{len(late)} names in block >= 1 resolve", late_ok)

        long_names = [n for n in truth_names.values() if len(n) >= 512]
        if long_names:
            found = [n for n in long_names if n in got.values()]
            check(f"{len(long_names)} name(s) >= 512 chars survive",
                  len(found) == len(long_names),
                  "signed header read would turn these into 'None'")

    # ---- Objects.txt ------------------------------------------------------
    print("Objects.txt")
    objs_file = dump / "Objects.txt"
    if not objs_file.exists():
        check("file exists", False)
    else:
        lines = [l for l in objs_file.read_text(errors="replace").splitlines() if l.strip()]
        check(f"object count == {expect['object_count']}",
              len(lines) == expect["object_count"], f"got {len(lines)}")
        text = "\n".join(lines)
        for needed in ("Engine.Actor", "Engine.Character", "MyGame.BP_Hero_C",
                       "MyGame.LateBlockClass"):
            check(f"path '{needed}' present", needed in text)

    # ---- SDK --------------------------------------------------------------
    print("SDK/Game_classes.hpp")
    sdk = dump / "SDK" / "Game_classes.hpp"
    if not sdk.exists():
        check("file exists", False)
    else:
        src = sdk.read_text(errors="replace")
        check("ACharacter derives from APawn",
              re.search(r"class ACharacter\s*:\s*public APawn", src) is not None)
        check("ABP_Hero_C derives from ACharacter",
              re.search(r"class ABP_Hero_C\s*:\s*public ACharacter", src) is not None)
        check("Health at 0x0400",
              re.search(r"Health;\s*//\s*0x0400", src) is not None)
        # upstream reads ArrayDim at ElementSize's offset, which would make
        # this [8] instead of [4]
        check("InventorySlots has ArrayDim 4",
              re.search(r"InventorySlots\[4\];", src) is not None,
              "would be [8] if ArrayDim were read at 0x34")
        check("Loadout has ArrayDim 3",
              re.search(r"Loadout\[3\];", src) is not None)
        # BP_Hero_C carries FProperty on ChildProperties (the 4.25+/UE5
        # layout). A dumper that only walks Children emits this class empty.
        check("FProperty path: bIsSprinting at 0x0520",
              re.search(r"bIsSprinting;\s*//\s*0x0520", src) is not None,
              "class would have no members if ChildProperties were skipped")
        check("FProperty path: Stamina at 0x0500",
              re.search(r"Stamina;\s*//\s*0x0500", src) is not None)

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED: {', '.join(failures)}")
        return 1
    print("all checks passed")
    return 0

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
