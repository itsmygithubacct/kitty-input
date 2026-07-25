# kitty-input

`kitty-input` is a small, allocation-free C11 input layer for terminal games
and applications. It preserves wire order across:

- Kitty and legacy keyboard events
- SGR 1006/1016 mouse press, release, motion, and wheel events
- terminal focus-in/focus-out events
- bracketed-paste chunks

Keyboard decoding and reliable held-key state come from the pinned
[`kitty-keyboard`](https://github.com/itsmygithubacct/kitty-keyboard)
submodule. The static and shared libraries bundle both layers, so consumers
link one library.

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
git clone --recurse-submodules https://github.com/itsmygithubacct/kitty-input.git
cd kitty-input
make test
make sanitize
```

`make` builds `build/libkitty-input.a`, `build/libkitty-input.so`, and the
`build/events` example. Run the example in a terminal and press `q` to exit.

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
bounded time since the previous action update.
`KITTYIN_GAMEPAD_ANY` is intended for single-player mappings. Specific
controller slots can be bound independently. A disconnect releases every
action held by that device before active-device selection falls back to
another connected controller or the keyboard.

## Guarantees

- no heap allocation or global parser state
- bounded queues and paste chunks with drop counters
- zero-based mouse coordinates
- malformed SGR mouse packets never leak into text input
- OSC/DCS/PM/APC terminal responses are filtered
- held-key state is reported only after release-capable Kitty flags are known
- safe reuse with an externally owned `kittykb_input`

The public API is versioned as `0.2.0` through `KITTYIN_VERSION_*`.

## License

MIT
