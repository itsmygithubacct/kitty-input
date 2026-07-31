# kitty-keyboard

`kitty-keyboard` is a small C11 library for the [Kitty keyboard
protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/). It decodes
fragmented terminal input, exposes press, repeat, and release events, and tracks
each held key independently.

The parser is allocation-free and portable ISO C. An optional POSIX layer owns
terminal setup, capability detection, non-blocking reads, and restoration.

## Build and test

```sh
make
make test
make sanitize
./build/held-keys
```

The final command runs an interactive key-state monitor. Hold several WASD or
arrow keys, then release them independently. The test suite covers fragmented
sequences, modifiers, text payloads, legacy encodings, malformed input, queue
overflow, simultaneous holds, and terminal lifecycle behavior through a PTY.

## Terminal input

```c
#include "kitty_keyboard.h"
#include "kitty_keyboard_posix.h"

kittykb_terminal keyboard;
kittykb_terminal_options options;

kittykb_terminal_init(&keyboard);
kittykb_terminal_options_init(&options);

if (kittykb_terminal_start(&keyboard, STDIN_FILENO, STDOUT_FILENO,
                           &options) != 0) {
    /* errno describes the failure. */
}

/* Call from the application's input loop. */
kittykb_terminal_read(&keyboard);

kittykb_event event;
while (kittykb_input_next(&keyboard.input, &event)) {
    if (event.action == KITTYKB_ACTION_PRESS) {
        /* Handle edge-triggered actions. */
    }
}

int horizontal =
    (kittykb_input_key_down(&keyboard.input, 'd') ? 1 : 0) -
    (kittykb_input_key_down(&keyboard.input, 'a') ? 1 : 0);

kittykb_terminal_stop(&keyboard);
```

The default terminal helper:

- saves and enables raw terminal mode;
- makes the input descriptor non-blocking;
- pushes protocol flags for disambiguation, event types, alternate keys,
  all-key reporting, and associated text;
- queries the active flags and bounds capability detection with primary device
  attributes;
- filters terminal replies and control strings out of the event queue;
- pops the keyboard mode and restores descriptor and terminal state on stop.

Call `kittykb_terminal_start()` after entering the main or alternate screen
whose keyboard-mode stack should change. Call `kittykb_terminal_stop()` before
leaving that screen.

Applications that already own raw mode or descriptor flags can disable those
parts of setup:

```c
kittykb_terminal_options_init(&options);
options.make_raw = false;
options.make_nonblocking = false;
```

## Held-key contract

`kittykb_input_key_down()` and `kittykb_input_held_count()` return meaningful
state only after the terminal confirms both event-type and all-key reporting.
On a legacy press-only stream, they return `false` and zero rather than risk a
key becoming permanently stuck.

Use `kittykb_input_has_release_events()` to select an application-specific
fallback when enhanced reporting is unavailable.

## Parser-only use

Applications that own their file descriptors can use only
`kitty_keyboard.h`:

```c
kittykb_input input;
kittykb_input_init(&input);
kittykb_input_feed(&input, bytes, byte_count);
```

Input may be divided at arbitrary byte boundaries. Use
`kittykb_terminal_push_flags()`, `kittykb_terminal_query_flags()`, and
`kittykb_terminal_pop_flags()` when only protocol negotiation is needed from
the POSIX layer.

A lone legacy Escape byte is inherently ambiguous. Call
`kittykb_input_flush_escape()` after the application's Escape timeout.
Enhanced mode reports Escape as an unambiguous CSI-u event.

## Event model

`kittykb_event` contains:

- an unshifted Unicode key or `KITTYKB_KEY_*` functional value;
- shifted and base-layout alternatives;
- Shift, Alt, Ctrl, Super, Hyper, Meta, Caps Lock, and Num Lock modifiers;
- a press, repeat, or release action;
- up to 16 associated text code points;
- the input source and whether the action was explicit on the wire.

Functional values use Kitty's canonical private-use mapping. Legacy arrow,
function-key, UTF-8, Alt, and CSI-u control-key encodings are normalized into
the same event model.

The input and terminal objects are intended to be owned by one input thread.
Queue overflow drops queued events but continues updating held state; counters
returned by `kittykb_input_get_stats()` expose all drops and malformed input.

## Install

```sh
make install PREFIX=/usr/local
```

The build produces static and shared libraries. Applications may instead
compile `src/kitty_keyboard.c` and `src/kitty_keyboard_posix.c` directly. The
library has no dependencies beyond the C and POSIX system libraries. PTY tests
use `libutil`; applications do not.

The API is pre-1.0 and may change between minor releases.

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
