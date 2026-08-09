# kitty-input

`kitty-input` is a small, allocation-free C11 input layer for terminal games
and applications. It preserves wire order across:

- Kitty and legacy keyboard events
- SGR 1006/1016 mouse press, release, motion, and wheel events
- terminal focus-in/focus-out events
- bracketed-paste chunks

Keyboard decoding and reliable held-key state come from the bundled
[`kitty-keyboard`](third_party/kitty_keyboard) component. Its source and
history live in this repository, and the static and shared libraries bundle
both layers so consumers link one library.

The same allocation-free API also provides:

- consumer-defined semantic actions with keyboard, gamepad-button, and
  directional-axis bindings;
- runtime rebinding, active-device tracking, and deterministic held-action
  repeat;
- configurable analog dead zones and release-safe controller state; and
- nonblocking Linux joystick discovery for `/dev/input/js0` through `js3`,
  including connect and disconnect events. Other POSIX targets retain the
  portable action mapper and a no-device gamepad backend.

## Build and test

```sh
git clone https://github.com/itsmygithubacct/kitty-input.git
cd kitty-input
make test
make sanitize
make benchmark
make fuzz FUZZ_TIME=30
```

`make` builds `build/libkitty-input.a`, `build/libkitty-input.so`, and the
`build/events` example. Run the example in a terminal and press `q` to exit.

The imported keyboard component retains its standalone build and PTY test
suite:

```sh
make keyboard-all
make keyboard-test
make keyboard-sanitize
```

The benchmark reports median parser and action-map costs. The Clang fuzz
target differentially checks bulk and byte-fragmented feeds and verifies
semantic-action state/source invariants under ASan and UBSan; `FUZZ_TIME` is
the run length in seconds.

## Parser-only use

```c
kittyin_input input;
kittyin_event event;

kittyin_input_init(&input);
kittyin_input_feed(&input, bytes, byte_count);
while (kittyin_input_next(&input, &event)) {
    /* Dispatch event.kind. */
}
```

Input may be fragmented at any byte boundary. Mouse and focus-looking bytes
inside bracketed paste are emitted only as paste data. Paste payloads larger
than `KITTYIN_PASTE_CHUNK_CAPACITY` are split into ordered chunks; the last
chunk has `final == true`.

## POSIX lifecycle

`kittyin_terminal_start()` owns raw mode, nonblocking input, Kitty keyboard
negotiation, mouse tracking, focus reporting, and bracketed paste until the
matching `kittyin_terminal_stop()`. Defaults enable button mouse tracking,
pixel coordinates when supported, focus events, and bracketed paste. Choose
drag or all-motion reporting with `kittyin_terminal_options.mouse_tracking`.

The lifecycle is intentionally explicit: start after entering the terminal
screen that owns the modes, and stop before leaving it. Signal handlers can
call `kittyin_terminal_emergency_restore()` for a best-effort mode reset.

## Semantic actions and gamepads

Applications choose their own bounded action IDs and may attach more than one
binding to each:

```c
kittyin_action_map actions;
kittyin_action_event mapped[8];
kittyin_gamepads gamepads;
kittyin_gamepad_event gamepad;
size_t count;

kittyin_action_map_init(&actions);
kittyin_action_map_bind_key(&actions, 0, KITTYKB_KEY_ENTER, 0);
kittyin_action_map_bind_gamepad_button(
    &actions, 0, KITTYIN_GAMEPAD_ANY, 0);
kittyin_action_map_bind_gamepad_axis(
    &actions, 1, KITTYIN_GAMEPAD_ANY, 1, -1);

kittyin_gamepads_init(&gamepads);
kittyin_gamepads_poll(&gamepads, now_ms);
while (kittyin_gamepads_next(&gamepads, &gamepad)) {
    count = kittyin_action_map_gamepad(
        &actions, &gamepad, mapped, 8);
    /* Dispatch mapped[0..count). */
}
count = kittyin_action_map_advance(&actions, elapsed_ms, mapped, 8);
kittyin_gamepads_close(&gamepads);
```

`now_ms` is the caller's monotonic millisecond clock and `elapsed_ms` is the
time since the previous action update, up to `UINT32_MAX`. Missed repeat
intervals are coalesced in constant time: each `advance()` call emits at most
one repeat per held action rather than a catch-up burst.
`KITTYIN_GAMEPAD_ANY` is intended for single-player mappings. Specific
controller slots can be bound independently. A disconnect releases every
action held by that device before active-device selection falls back to
another connected controller or the keyboard.

Legacy uppercase letters match their lowercase no-modifier bindings, and raw
C0 encodings such as byte `0x03` match the equivalent `Ctrl+C` semantic
binding. Initial Linux joystick snapshots establish no actions; only later
changes synthesize presses. `kittyin_gamepads_attach_fd()` takes ownership of
a successful descriptor attachment and makes it nonblocking and close-on-exec.

Action output functions return the number actually stored, never more than
the supplied capacity. State transitions still occur when capacity is zero or
exhausted, so applications that must observe every transition should provide
room for their maximum simultaneously matching actions.

## Guarantees

- no heap allocation or global parser state
- bounded queues and paste chunks with drop counters
- disconnect retention under gamepad queue pressure (the oldest queued sample
  is evicted and counted as dropped)
- zero-based mouse coordinates
- malformed SGR mouse packets never leak into text input
- OSC/DCS/SOS/PM/APC terminal responses are filtered with BEL/ST-specific
  termination rules and CAN/SUB cancellation
- held-key state is reported only after release-capable Kitty flags are known
- safe reuse with an externally owned `kittykb_input`

The public API is versioned as `0.2.1` through `KITTYIN_VERSION_*`. The bundled
keyboard component is versioned as `0.1.1` through `KITTYKB_VERSION_*`.

## License

MIT
