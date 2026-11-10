# kitty-input for Python

The `kitty-input` parser for Python applications: terminal bytes in, ordered
keyboard, mouse, focus and paste events out. Standard library only, Python
3.11 or newer, no native library.

```python
import os
from kitty_input import Input, KeyEvent, MouseEvent, MOUSE_PRESS

decoder = Input()
decoder.feed(os.read(fd, 4096))
if decoder.has_pending_escape:
    ...                      # wait ~25 ms for more bytes, then:
    decoder.flush_escape()   # a lone ESC was the Escape key
for event in decoder:
    if isinstance(event, KeyEvent) and event.pressed:
        print(event.chord, event.text)        # "ctrl+s", "" / "a", "a"
    elif isinstance(event, MouseEvent) and event.action == MOUSE_PRESS:
        print(event.button, event.x, event.y)  # zero-based cells
```

## Why a port and not a binding

Every Python application on this stack is standard-library-only and
architecture-independent. A `ctypes` binding would make each of them carry,
find and version a native library to parse a few hundred bytes a second. The
parser is small, so this package re-implements it and holds itself to the C
contract instead:

- the same event model (`KeyEvent`, `MouseEvent`, `FocusEvent`,
  `PasteEvent`) with the same fields;
- the same key codes and modifier bits (`kitty_input.keys` copies
  `kitty_keyboard.h`);
- the same two-layer structure: an outer layer that owns mouse, focus, paste
  and control strings and hands every other sequence to an inner keyboard
  layer, which re-parses it. That is what makes the two agree on malformed
  input, 8-bit C1 controls and a sequence interrupted by text.

`make python-diff` builds a small C program against the library and feeds
both decoders the same random wire, split at a random point, comparing every
event field by field. It is part of `make python-test` when a C compiler is
available.

## What it decodes

- Kitty keyboard protocol keys (`CSI code:shifted:base;mods:action;text u`),
  including release and repeat events.
- Legacy keys: UTF-8 text, C0 controls, `ESC x` as alt, CSI letter and tilde
  keys with modifiers, SS3.
- SGR 1006 mouse press, release, motion and wheel, with the 1016 pixel unit
  recorded when you ask for it.
- Focus in and out, bracketed paste in bounded chunks.
- OSC, DCS, SOS, PM and APC strings are consumed, so terminal replies never
  appear as typing. CAN and SUB cancel a sequence; a fresh ESC resynchronises.

`KeyEvent` is wire-faithful: a legacy terminal's ctrl+s arrives as key `0x13`
with no modifiers, exactly as in C. `event.normalized()` and `event.chord`
fold that onto the Kitty meaning (`s` with `CTRL`), so an application can
bind `"ctrl+s"` once. `keys.parse_chord()` is the inverse.

## Terminal modes

The decoder never writes to the terminal. `kitty_input.terminal` holds the
mode strings an application sends on entry and exit: `MOUSE_ON`/`MOUSE_OFF`,
`PASTE_ON`/`PASTE_OFF`, `FOCUS_ON`/`FOCUS_OFF`, and the Kitty keyboard
protocol push/pop.

## Tests

```sh
make -C .. python-test   # unit tests, then the differential fuzz against C
```
