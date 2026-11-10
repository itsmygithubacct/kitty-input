# kitty-input

`kitty-input` is a small, allocation-free C11 library that decodes terminal
keyboard, mouse, focus and paste input in wire order, and maps keys and gamepads
to application-defined actions.

It is not installed on its own. It reaches programs through
[kitty-terminal-session](https://github.com/itsmygithubacct/kitty-terminal-session#readme),
which pins it as a submodule; Kilix games and apps compile it into their
binaries. Most of those install on demand, from the Kilix content catalog or the
first time a Kilix command needs them; the PDF Viewer (kilix-pdf) is installed
at first boot.

## What it does

- Decodes Kitty-protocol and legacy keyboard events with press, repeat and
  release, and tracks every held key independently.
- Decodes SGR 1006/1016 mouse press, release, motion and wheel events, focus
  in/out, and bracketed paste in ordered chunks, all in the order they arrived.
- Filters terminal responses (OSC, DCS, APC and the rest) so they never leak into
  text input, and accepts input fragmented at any byte boundary.
- Owns the terminal modes it needs (raw mode, Kitty keyboard flags, mouse
  tracking, focus reporting, bracketed paste) between an explicit start and stop,
  with an emergency restore for signal handlers.
- Maps keys, gamepad buttons and directional axes to consumer-defined actions,
  with rebinding, dead zones, held-action repeat and active-device tracking.
- Discovers Linux joysticks (`/dev/input/js0` to `js3`) without blocking,
  including connect and disconnect.
- Bundles the [kitty-keyboard](third_party/kitty_keyboard/) decoder; one static
  or shared library contains both layers.

## Quick start

```sh
git clone https://github.com/itsmygithubacct/kitty-input.git
cd kitty-input
make            # build/libkitty-input.a, .so and build/events
make test       # parser tests, keyboard component tests, benchmark assertions
./build/events  # print decoded events; q exits
```

```c
#include "kitty_input.h"

kittyin_input input;
kittyin_event event;
kittyin_input_init(&input);
kittyin_input_feed(&input, bytes, byte_count);
while (kittyin_input_next(&input, &event)) {
    /* dispatch event.kind */
}
```

## Usage

| Area | Main calls |
|---|---|
| Parser | `kittyin_input_init`, `kittyin_input_feed`, `kittyin_input_next`, `kittyin_input_key_down`, `kittyin_input_held_count`, `kittyin_input_flush_escape`, `kittyin_input_get_stats` |
| Terminal lifecycle (`kitty_input_posix.h`) | `kittyin_terminal_options_init`, `kittyin_terminal_init`, `kittyin_terminal_start`, `kittyin_terminal_read`, `kittyin_terminal_stop`, `kittyin_terminal_emergency_restore` |
| Actions | `kittyin_action_map_init`, `kittyin_action_map_bind_key`, `kittyin_action_map_bind_gamepad_button`, `kittyin_action_map_bind_gamepad_axis`, `kittyin_action_map_key`, `kittyin_action_map_gamepad`, `kittyin_action_map_advance`, `kittyin_action_map_set_repeat`, `kittyin_action_map_set_dead_zone` |
| Gamepads | `kittyin_gamepads_init`, `kittyin_gamepads_poll`, `kittyin_gamepads_next`, `kittyin_gamepads_attach_fd`, `kittyin_gamepads_close` |

| Make target | What it does |
|---|---|
| `make` | static and shared libraries plus the `events` example |
| `make test` | the input tests, the keyboard component's PTY tests, and benchmark assertions |
| `make sanitize` | the tests under AddressSanitizer and UndefinedBehaviorSanitizer |
| `make benchmark` | median parser and action-map costs |
| `make fuzz FUZZ_TIME=30` | Clang libFuzzer differential fuzzing for the given number of seconds |
| `make keyboard-all`, `make keyboard-test`, `make keyboard-sanitize` | the bundled keyboard component's own build and tests |

## How it works

```
 terminal bytes ──▶ kittyin_input_feed ──▶ kitty-keyboard decoder ─┐
                    (any fragmentation)    SGR mouse, focus, paste ─┼─▶ ordered event queue ──▶ kittyin_input_next
                                           terminal responses: dropped
 key / gamepad events ──▶ kittyin_action_map ──▶ action press, repeat, release
```

The parser keeps no global state and allocates nothing: queues and paste chunks
are fixed-size, and overflow is counted rather than allocated. Mouse- or
focus-looking bytes inside a bracketed paste stay paste data, and held-key state
is reported only once the terminal has confirmed release-capable Kitty flags.
The action map turns key and gamepad events into the application's own action
ids; repeat intervals missed between updates are coalesced, so a slow frame
produces one repeat rather than a burst, and a disconnected controller releases
every action it held.

Depth: [docs/api.md](docs/api.md).

## Python

[`python/`](python/README.md) is a standard-library port of this parser for
Python applications, with the same event model and key codes. It is a port
rather than a binding so that pure-Python applications need no native
library; `make python-test` runs its unit tests and a differential fuzz that
feeds the same random wire to the C and Python decoders and compares every
event.

## Documentation

- [docs/api.md](docs/api.md): parser-only use, the POSIX lifecycle, semantic
  actions and gamepads, and the guarantees and version numbers.
- [docs/building.md](docs/building.md): build, test, sanitizer, benchmark and
  fuzz targets.
- [third_party/kitty_keyboard/README.md](third_party/kitty_keyboard/README.md):
  the bundled Kitty keyboard protocol decoder.

## Related repositories

- [kitty-terminal-session](https://github.com/itsmygithubacct/kitty-terminal-session#readme)
  pins this library at `third_party/kitty-input` beside kitty-framebuffer. Every
  program that pins that session compiles kitty-input in:
  [kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk#readme) and
  the games built on it
  ([kilix-games](https://github.com/itsmygithubacct/kilix-games#readme),
  [pleb-tower](https://github.com/itsmygithubacct/pleb-tower#readme),
  [super-kilix](https://github.com/itsmygithubacct/super-kilix#readme),
  [pleb-plant-grower](https://github.com/itsmygithubacct/pleb-plant-grower#readme),
  [kilix-land](https://github.com/itsmygithubacct/kilix-land#readme)), and
  [kilix-techno](https://github.com/itsmygithubacct/kilix-techno#readme),
  [kilix-pdf](https://github.com/itsmygithubacct/kilix-pdf#readme),
  [kilix-playalong](https://github.com/itsmygithubacct/kilix-playalong#readme),
  [kilix-rtsp](https://github.com/itsmygithubacct/kilix-rtsp#readme),
  [kilix-sound-detect](https://github.com/itsmygithubacct/kilix-sound-detect#readme)
  and [kilix-mask](https://github.com/itsmygithubacct/kilix-mask#readme).
  Several games include `kitty_keyboard.h` from the bundled decoder directly.
- [kitty-framebuffer](https://github.com/itsmygithubacct/kitty-framebuffer#readme)
  is the matching presentation library.

## Part of Plebian-OS

kitty-input is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It reaches a user compiled, through kitty-terminal-session, into Kilix games
and apps: most install on demand, and the PDF Viewer is installed at first boot.
The map of every repository and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT
