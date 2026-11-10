"""Differential fuzz: the Python decoder against the C decoder, event for event.

Skipped unless `KITTY_INPUT_DUMP` names the C dumper that `make python-diff`
builds from `tests/differential/dump_events.c`. The unit tests pin the
contract; this is what shows the two implementations still agree on input
nobody thought to write down.
"""

from __future__ import annotations

import os
import random
import subprocess
import unittest

from kitty_input import FocusEvent, Input, KeyEvent, MouseEvent, PasteEvent

DUMP = os.environ.get("KITTY_INPUT_DUMP", "")
CASES = int(os.environ.get("KITTY_INPUT_DIFF_CASES", "2000"))

ALPHABET = (
    b"\x1b\x1b\x1b[[[]<;;:0123456789MmuABCDEFHPQRSZ~OIc?>"
    b"\x07\x9c\x18\x1a\\_^PX\r\n\t\x7f\x08\x00\x01\x13 a"
    b"\xc3\xa9\xe6\x97\xa5\xf0\x9f\x98\x80\xff\x80"
)
FRAGMENTS = (
    b"\x1b[200~", b"\x1b[201~", b"\x1b[<", b"\x1b[97;5u", b"\x1b[1;5A",
    b"\x1b]0;t\x07", b"\x1bP", b"\x1b\\", b"\x1b[I", b"\x1b[O", b"\x1bO",
    b"\x1b[<64;3;4M", b"\x1b[3~", b"\x1b[?1u",
)


def python_lines(wire: bytes, cut: int, pixels: bool) -> list[str]:
    decoder = Input(pixel_coordinates=pixels)
    out: list[str] = []
    for part in (wire[:cut], wire[cut:]):
        decoder.feed(part)
        out.extend(_line(e) for e in decoder.events())
    decoder.flush_escape()
    out.extend(_line(e) for e in decoder.events())
    return out


def _line(event: object) -> str:
    if isinstance(event, KeyEvent):
        fields = [event.key, event.modifiers, event.action, event.shifted_key,
                  event.base_layout_key, event.source,
                  int(event.event_type_explicit)]
        fields += [ord(c) for c in event.text]
        return "key " + " ".join(map(str, fields))
    if isinstance(event, MouseEvent):
        fields = [event.x, event.y, event.button, event.action,
                  event.modifiers, event.wheel_x, event.wheel_y,
                  event.raw_button, int(event.pixel_coordinates)]
        return "mouse " + " ".join(map(str, fields))
    if isinstance(event, FocusEvent):
        return f"focus {int(event.focused)}"
    if isinstance(event, PasteEvent):
        return " ".join(["paste", str(int(event.final))]
                        + [str(b) for b in event.data])
    raise TypeError(event)


def random_wire(rng: random.Random) -> bytes:
    parts = []
    for _ in range(rng.randint(0, 24)):
        if rng.random() < 0.3:
            parts.append(rng.choice(FRAGMENTS))
        else:
            parts.append(bytes([rng.choice(ALPHABET)]))
    return b"".join(parts)


@unittest.skipUnless(DUMP and os.access(DUMP, os.X_OK),
                     "set KITTY_INPUT_DUMP (make python-diff)")
class Differential(unittest.TestCase):
    def test_python_matches_c(self) -> None:
        rng = random.Random(int(os.environ.get("KITTY_INPUT_DIFF_SEED", "24")))
        mismatches = []
        for _ in range(CASES):
            wire = random_wire(rng)
            cut = rng.randint(0, len(wire))
            pixels = rng.random() < 0.5
            argv = [DUMP, str(cut)] + (["pixels"] if pixels else [])
            c_lines = subprocess.run(
                argv, input=wire, capture_output=True, check=True,
            ).stdout.decode().splitlines()
            py_lines = python_lines(wire, cut, pixels)
            if c_lines != py_lines:
                mismatches.append((wire, cut, c_lines, py_lines))
                if len(mismatches) >= 5:
                    break
        if mismatches:
            report = "\n".join(
                f"wire={w!r} cut={c}\n  C:  {cl}\n  Py: {pl}"
                for w, c, cl, pl in mismatches
            )
            self.fail(f"{len(mismatches)} mismatches\n{report}")


if __name__ == "__main__":
    unittest.main()
