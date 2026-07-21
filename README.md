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

## Guarantees

- no heap allocation or global parser state
- bounded queues and paste chunks with drop counters
- zero-based mouse coordinates
- malformed SGR mouse packets never leak into text input
- OSC/DCS/PM/APC terminal responses are filtered
- held-key state is reported only after release-capable Kitty flags are known
- safe reuse with an externally owned `kittykb_input`

The public API is versioned as `0.1.0` through `KITTYIN_VERSION_*`.

## License

MIT
