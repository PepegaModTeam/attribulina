#!/usr/bin/env python3
"""Names message_ids no header names, from their handlers' names, and adds
them to data/messages/inferred.txt.

A handler collection is named <state>_handler_<message, lower case, no M>
(the Sequence Builder names new ones that way too): message MNotifyRoundOver
gets ..._handler_notifyroundover. Its message_id is StringHash32 of the
message name. So for each unnamed id this splits the handler's suffix into
words (the words of every known message name, as written there: "NIS",
"Race", then with one other word allowed), prefixes M, and keeps the
spelling whose hash is the id; failing that it tries every capitalized
split of suffixes up to 22 letters. Nothing
is added that the hash doesn't confirm. The games share message names, so
the names apply to every game.

    attribcli export <database> gameplay --children --field message_id -o handlers.amod
    python3 scripts/infer_messages.py <game_platform> handlers.amod [...]

The first argument (undercover_wii) only labels the names in the file.
"""
import itertools
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from gen_schema import lookup2  # noqa: E402

MESSAGES = ROOT / "data" / "messages"
INFERRED = MESSAGES / "inferred.txt"


def known_names():
    names = set()
    for f in sorted(MESSAGES.glob("*.txt")):
        for line in f.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                names.add(line.split()[0])
    return names


def vocabulary(names):
    words = defaultdict(set)
    for n in names:
        for w in re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+", n[1:]):
            words[w.lower()].update({w, w.capitalize()})
    return words


def splits(s, words, memo, unknown):
    """Every split of s into known words, with up to `unknown` other words."""
    key = (s, unknown)
    if key in memo:
        return memo[key]
    out = [[]] if not s else []
    for i in range(1, len(s) + 1):
        if s[:i] in words:
            out += [[s[:i]] + rest for rest in splits(s[i:], words, memo, unknown)]
        elif unknown:
            out += [[s[:i]] + rest for rest in splits(s[i:], words, memo, unknown - 1)]
    memo[key] = out
    return out


def infer(mid, suffix, words, memo):
    # Known words first, then one word no message name has (in either case).
    for unknown in (0, 1):
        for parts in splits(suffix, words, memo, unknown):
            forms = [sorted(words[w]) if w in words else [w.capitalize(), w.upper()] for w in parts]
            for spelled in itertools.product(*forms):
                name = "M" + "".join(spelled)
                if lookup2(name) == mid:
                    return name
    if len(suffix) <= 22:
        for mask in range(1 << (len(suffix) - 1)):
            parts, cur = [], suffix[0]
            for i in range(1, len(suffix)):
                if mask >> (i - 1) & 1:
                    parts.append(cur)
                    cur = suffix[i]
                else:
                    cur += suffix[i]
            name = "M" + "".join(p.capitalize() for p in parts + [cur])
            if lookup2(name) == mid:
                return name
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    label, exports = sys.argv[1], sys.argv[2:]
    names = known_names()
    by_hash = {lookup2(n): n for n in names}
    words = vocabulary(names)
    ids = defaultdict(set)
    for f in exports:
        text = Path(f).read_text(errors="replace")
        for m in re.finditer(r"collection gameplay/(\S+)[^\n]*\{\s*message_id = (\d+)", text):
            mid, handler = int(m.group(2)), m.group(1)
            if mid and "_handler_" in handler and mid not in by_hash:
                ids[mid].add(handler.split("_handler_")[-1])
    found, missing = [], []
    memo = {}
    for mid, suffixes in sorted(ids.items()):
        name = next((n for s in sorted(suffixes) if (n := infer(mid, s, words, memo))), None)
        (found if name else missing).append((mid, name, sorted(suffixes)))
    if found:
        new = not INFERRED.exists()
        with INFERRED.open("a", newline="\n") as fh:
            if new:
                fh.write("# Message names no header has, found from their handlers' names by\n")
                fh.write("# scripts/infer_messages.py; StringHash32 of each is the message_id it names.\n")
            fh.write(f"# {label}\n")
            for mid, name, _ in found:
                fh.write(f"{name}\n")
    for mid, name, suffixes in found:
        print(f"0x{mid:08X} {name}  (handler_{suffixes[0]})")
    for mid, _, suffixes in missing:
        print(f"0x{mid:08X} not found  (handler_{', handler_'.join(suffixes)})")
    print(f"{len(found)} added to {INFERRED.relative_to(ROOT)}, {len(missing)} not found")


if __name__ == "__main__":
    main()
