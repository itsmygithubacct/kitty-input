"""The wire decoder: bytes from a terminal in, ordered events out.

This is a port of the parser in `src/kitty_input.c` and the bundled
`kitty_keyboard.c`, not a binding to them. Python applications on this stack
are standard-library-only and arch-independent, and a decoder is small enough
that a second implementation of the same contract costs less than shipping a
native library to every one of them. The contract is the C one:

- keyboard events from the Kitty protocol (``CSI code;mods:action;text u``)
  and from legacy encodings (C0 bytes, UTF-8, ``ESC x`` as alt, CSI letter and
  tilde keys, SS3);
- SGR 1006 mouse reports, with 1016 pixel coordinates selectable;
- focus in/out (``CSI I`` / ``CSI O``);
- bracketed paste, delivered in bounded chunks, with a failed terminator
  prefix returned byte for byte;
- OSC, DCS, SOS, PM and APC strings consumed and ignored, so a terminal reply
  never leaks into the text stream;
- CAN and SUB cancel a sequence, and a fresh ESC resynchronises.

Feeding a buffer whole or one byte at a time yields the same events; the test
suite checks that on the same wire the C suite uses.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from typing import Iterator, Union

from . import keys as K

__all__ = [
    "Event",
    "FocusEvent",
    "Input",
    "KeyEvent",
    "MouseEvent",
    "PasteEvent",
    "Stats",
    "decode",
    "MOUSE_PRESS",
    "MOUSE_RELEASE",
    "MOUSE_MOVE",
    "MOUSE_WHEEL",
]

MOUSE_PRESS = 1
MOUSE_RELEASE = 2
MOUSE_MOVE = 3
MOUSE_WHEEL = 4

SEQUENCE_CAPACITY = 256
PASTE_CHUNK_CAPACITY = 128
MAX_TEXT_CODEPOINTS = 16  # KITTYKB_MAX_TEXT_CODEPOINTS

_PASTE_END = b"\x1b[201~"

(
    _GROUND, _ESCAPE, _CSI, _SS3, _OSC, _OSC_ESCAPE, _STRING, _STRING_ESCAPE,
    _CSI_DISCARD, _SS3_DISCARD, _PASTE,
) = range(11)


@dataclass(frozen=True)
class KeyEvent:
    key: int
    modifiers: int = 0
    action: int = K.PRESS
    text: str = ""
    shifted_key: int = 0
    base_layout_key: int = 0
    source: int = K.SOURCE_LEGACY
    event_type_explicit: bool = False

    kind = "key"

    @property
    def pressed(self) -> bool:
        """Press or repeat: the actions an editor acts on."""
        return self.action != K.RELEASE

    @property
    def chord(self) -> str:
        """``ctrl+s`` and friends, after `normalized()`."""
        event = self.normalized()
        return K.chord_name(event.key, event.modifiers & K.CHORD_MODIFIERS)

    def normalized(self) -> "KeyEvent":
        """Fold the legacy encodings onto the Kitty-protocol meaning.

        A legacy terminal sends ctrl+a as byte 0x01, and the wire decoder
        reports exactly that (key 1, no modifiers), as the C decoder does.
        Applications want one spelling, so this maps C0 letters to the letter
        with CTRL and NUL to ctrl+space. Enter, Tab, Backspace and Escape
        already decode to their named keys, so 0x08, 0x09, 0x0D and 0x1B are
        not touched here.
        """
        key = self.key
        if 1 <= key <= 26 and key not in (8, 9, 10, 13):
            return KeyEvent(
                key=ord("a") + key - 1,
                modifiers=self.modifiers | K.CTRL,
                action=self.action,
                text="",
                source=self.source,
            )
        if key == 0:
            return KeyEvent(key=0x20, modifiers=self.modifiers | K.CTRL,
                            action=self.action, source=self.source)
        if 28 <= key <= 31:  # ctrl+\ ctrl+] ctrl+^ ctrl+_
            return KeyEvent(key=ord("\\]^_"[key - 28]),
                            modifiers=self.modifiers | K.CTRL,
                            action=self.action, source=self.source)
        return self


@dataclass(frozen=True)
class MouseEvent:
    x: int
    y: int
    button: int = 0
    action: int = MOUSE_PRESS
    modifiers: int = 0
    wheel_x: int = 0
    wheel_y: int = 0
    raw_button: int = 0
    pixel_coordinates: bool = False

    kind = "mouse"


@dataclass(frozen=True)
class FocusEvent:
    focused: bool

    kind = "focus"


@dataclass(frozen=True)
class PasteEvent:
    data: bytes
    final: bool

    kind = "paste"

    @property
    def text(self) -> str:
        return self.data.decode("utf-8", "replace")


Event = Union[KeyEvent, MouseEvent, FocusEvent, PasteEvent]


@dataclass
class Stats:
    bytes_fed: int = 0
    events_decoded: int = 0
    malformed_sequences: int = 0
    ignored_sequences: int = 0
    paste_chunks: int = 0


def _cancels(byte: int) -> bool:
    return byte in (0x18, 0x1A)


def _text_allowed(value: int) -> bool:
    return (
        0x20 <= value <= 0x10FFFF
        and not 0x7F <= value <= 0x9F
        and not 0xD800 <= value <= 0xDFFF
    )


def _scalar(value: int) -> bool:
    return 0 <= value <= 0x10FFFF and not 0xD800 <= value <= 0xDFFF


def _decimal(text: str) -> int | None:
    if not text or not text.isascii() or not text.isdigit():
        return None
    value = int(text)
    return value if value <= 0xFFFFFFFF else None


def _canonical(key: int) -> int:
    return {27: K.ESCAPE, 13: K.ENTER, 9: K.TAB, 127: K.BACKSPACE}.get(key, key)


_LETTER_KEYS = {
    "A": K.UP, "B": K.DOWN, "C": K.RIGHT, "D": K.LEFT, "E": K.KP_BEGIN,
    "F": K.END, "H": K.HOME, "P": K.F1, "Q": K.F(2), "S": K.F(4),
    "Z": K.TAB,
}
_SS3_KEYS = dict(_LETTER_KEYS, R=K.F(3))
del _SS3_KEYS["Z"]
_TILDE_KEYS = {
    2: K.INSERT, 3: K.DELETE, 5: K.PAGE_UP, 6: K.PAGE_DOWN, 7: K.HOME,
    8: K.END, 11: K.F1, 12: K.F(2), 13: K.F(3), 14: K.F(4), 15: K.F(5),
    17: K.F(6), 18: K.F(7), 19: K.F(8), 20: K.F(9), 21: K.F(10),
    23: K.F(11), 24: K.F(12), 29: K.MENU,
}


def _modifier_action(text: str) -> tuple[int, int, bool] | None:
    """``mods[:action]`` -> (modifiers, action, explicit)."""
    if not text:
        return 0, K.PRESS, False
    mods_text, colon, action_text = text.partition(":")
    if ":" in action_text:
        return None
    encoded = _decimal(mods_text)
    if encoded is None or not 1 <= encoded <= 256:
        return None
    action, explicit = K.PRESS, False
    if colon:
        value = _decimal(action_text)
        if value is None or not 1 <= value <= 3:
            return None
        action, explicit = value, True
    return encoded - 1, action, explicit


def _parse_kitty_key(body: str) -> KeyEvent | None:
    fields = body.split(";")
    if len(fields) > 3:
        return None
    key_fields = fields[0].split(":")
    if len(key_fields) > 3:
        return None
    key = _decimal(key_fields[0])
    if key is None or not _scalar(key):
        return None
    shifted = base = 0
    if len(key_fields) > 1 and key_fields[1]:
        shifted = _decimal(key_fields[1])
        if shifted is None or not _scalar(shifted):
            return None
    if len(key_fields) > 2 and key_fields[2]:
        base = _decimal(key_fields[2])
        if base is None or not _scalar(base):
            return None
    modifiers, action, explicit = 0, K.PRESS, False
    if len(fields) > 1:
        parsed = _modifier_action(fields[1])
        if parsed is None:
            return None
        modifiers, action, explicit = parsed
    text = ""
    if len(fields) > 2 and fields[2]:
        codepoints = fields[2].split(":")
        if len(codepoints) > MAX_TEXT_CODEPOINTS:
            return None
        for item in codepoints:
            value = _decimal(item)
            if value is None or not _text_allowed(value):
                return None
            text += chr(value)
    key = _canonical(key)
    if key == K.NONE and not text:
        return None
    return KeyEvent(
        key=key, modifiers=modifiers, action=action, text=text,
        shifted_key=shifted, base_layout_key=base, source=K.SOURCE_KITTY,
        event_type_explicit=explicit,
    )


def _parse_legacy_key(body: str, final: str) -> KeyEvent | None:
    number_text, semicolon, rest = body.partition(";")
    if semicolon and ";" in rest:
        return None
    number = 1
    if number_text:
        value = _decimal(number_text)
        if value is None:
            return None
        number = value
    elif semicolon and not number_text:
        number = 1
    key = _TILDE_KEYS.get(number) if final == "~" else _LETTER_KEYS.get(final)
    if not key:
        return None
    if final != "~" and body and number != 1:
        return None
    modifiers, action, explicit = 0, K.PRESS, False
    if semicolon:
        parsed = _modifier_action(rest)
        if parsed is None:
            return None
        modifiers, action, explicit = parsed
    if final == "Z":
        modifiers |= K.SHIFT
    return KeyEvent(key=key, modifiers=modifiers, action=action,
                    source=K.SOURCE_LEGACY, event_type_explicit=explicit)


def _parse_mouse(body: str, final: str, pixels: bool) -> MouseEvent | None:
    parts = body[1:].split(";")
    if len(parts) != 3:
        return None
    values = [_decimal(part) for part in parts]
    if any(v is None for v in values):
        return None
    code, column, row = values  # type: ignore[misc]
    if column == 0 or row == 0 or column > 0x7FFFFFFF or row > 0x7FFFFFFF:
        return None
    modifiers = 0
    if code & 4:
        modifiers |= K.SHIFT
    if code & 8:
        modifiers |= K.ALT
    if code & 16:
        modifiers |= K.CTRL
    low = code & 3
    wheel_x = wheel_y = 0
    if code & 64:
        action = MOUSE_WHEEL
        button = 4 + low
        if low == 0:
            wheel_y = -1
        elif low == 1:
            wheel_y = 1
        elif low == 2:
            wheel_x = -1
        else:
            wheel_x = 1
    else:
        if code & 128:
            button = 8 + low
        elif low != 3:
            button = low + 1
        else:
            button = 0
        if code & 32:
            action = MOUSE_MOVE
        elif final == "m" or low == 3:
            action = MOUSE_RELEASE
        else:
            action = MOUSE_PRESS
    return MouseEvent(
        x=column - 1, y=row - 1, button=button, action=action,
        modifiers=modifiers, wheel_x=wheel_x, wheel_y=wheel_y,
        raw_button=code, pixel_coordinates=pixels,
    )


class _Keyboard:
    """The inner layer: a port of `kitty_keyboard.c`'s byte parser.

    The outer layer hands it either runs of ordinary bytes or one complete
    escape sequence at a time, and it re-parses what it is given with its own
    state. That two-step shape is what the C library does, and reproducing it
    -- rather than one merged state machine -- is what makes the two agree on
    malformed input, 8-bit C1 controls and a sequence interrupted by text.
    """

    def __init__(self) -> None:
        self.events: list[KeyEvent] = []
        self.stats = Stats()
        self.state = _GROUND
        self.string_osc = False
        self.sequence = bytearray()
        self.utf8_value = 0
        self.utf8_minimum = 0
        self.utf8_remaining = 0
        self.utf8_modifiers = 0

    def feed(self, data: bytes | bytearray) -> None:
        self.stats.bytes_fed += len(data)
        for byte in data:
            self.byte(byte)

    @property
    def pending_escape(self) -> bool:
        return self.state == _ESCAPE

    def flush_escape(self) -> None:
        if self.state == _ESCAPE:
            self.emit(KeyEvent(key=K.ESCAPE))
            self.state = _GROUND

    def emit(self, event: KeyEvent) -> None:
        self.stats.events_decoded += 1
        self.events.append(event)

    def codepoint(self, value: int, modifiers: int) -> None:
        text = chr(value) if _text_allowed(value) else ""
        self.emit(KeyEvent(key=value, modifiers=modifiers, text=text))

    def ground(self, byte: int, modifiers: int) -> None:
        if byte in (0x0D, 0x0A):
            self.emit(KeyEvent(key=K.ENTER, modifiers=modifiers))
        elif byte == 0x09:
            self.emit(KeyEvent(key=K.TAB, modifiers=modifiers))
        elif byte in (0x7F, 0x08):
            self.emit(KeyEvent(key=K.BACKSPACE, modifiers=modifiers))
        elif byte < 0x80:
            self.codepoint(byte, modifiers)
        elif 0xC2 <= byte <= 0xDF:
            self.utf8_start(byte & 0x1F, 0x80, 1, modifiers)
        elif 0xE0 <= byte <= 0xEF:
            self.utf8_start(byte & 0x0F, 0x800, 2, modifiers)
        elif 0xF0 <= byte <= 0xF4:
            self.utf8_start(byte & 0x07, 0x10000, 3, modifiers)
        else:
            self.stats.malformed_sequences += 1

    def utf8_start(self, value: int, minimum: int, remaining: int,
                   modifiers: int) -> None:
        self.utf8_value = value
        self.utf8_minimum = minimum
        self.utf8_remaining = remaining
        self.utf8_modifiers = modifiers

    def continuation(self, byte: int) -> None:
        if byte & 0xC0 != 0x80:
            self.utf8_remaining = 0
            self.stats.malformed_sequences += 1
            self.byte(byte)
            return
        self.utf8_value = (self.utf8_value << 6) | (byte & 0x3F)
        self.utf8_remaining -= 1
        if not self.utf8_remaining:
            value = self.utf8_value
            if value < self.utf8_minimum or not _scalar(value):
                self.stats.malformed_sequences += 1
            else:
                self.codepoint(value, self.utf8_modifiers)

    def start_string(self, osc: bool) -> None:
        self.state = _STRING
        self.string_osc = osc
        self.sequence.clear()

    def escape(self, byte: int) -> None:
        if byte == ord("["):
            self.state = _CSI
            self.sequence.clear()
        elif byte == ord("O"):
            self.state = _SS3
        elif byte == ord("]"):
            self.start_string(True)
        elif byte in b"P_^X":
            self.start_string(False)
        elif byte == ord("\\"):
            self.state = _GROUND
            self.stats.ignored_sequences += 1
        elif byte == 0x1B:
            self.emit(KeyEvent(key=K.ESCAPE, modifiers=K.ALT))
            self.state = _GROUND
        else:
            self.state = _GROUND
            self.ground(byte, K.ALT)

    def finish_csi(self, final: str) -> None:
        body = self.sequence.decode("latin-1")
        recognized = valid = False
        event: KeyEvent | None = None
        if final == "u" and body.startswith("?"):
            recognized = True
            valid = len(body) >= 2 and _decimal(body[1:]) is not None
        elif final == "u":
            recognized = True
            event = _parse_kitty_key(body)
            valid = event is not None
        elif final == "c" and body[:1] in ("?", ">"):
            recognized = valid = True
        elif final in "~ABCDEFHPQSZ":
            recognized = True
            event = _parse_legacy_key(body, final)
            valid = event is not None
        if event is not None:
            self.emit(event)
        if not recognized:
            self.stats.ignored_sequences += 1
        elif not valid:
            self.stats.malformed_sequences += 1
        self.state = _GROUND
        self.sequence.clear()

    def ss3(self, byte: int) -> None:
        key = _SS3_KEYS.get(chr(byte))
        if key is None:
            self.stats.ignored_sequences += 1
        else:
            self.emit(KeyEvent(key=key))
        self.state = _GROUND

    def byte(self, byte: int) -> None:  # noqa: C901 - one state machine
        if self.utf8_remaining:
            self.continuation(byte)
            return
        state = self.state
        if state == _GROUND:
            if byte == 0x1B:
                self.state = _ESCAPE
            elif byte == 0x9B:
                self.state = _CSI
                self.sequence.clear()
            elif byte == 0x9D:
                self.start_string(True)
            elif byte in (0x90, 0x98, 0x9E, 0x9F):
                self.start_string(False)
            else:
                self.ground(byte, 0)
        elif state == _ESCAPE:
            self.escape(byte)
        elif state == _CSI:
            if 0x40 <= byte <= 0x7E:
                self.finish_csi(chr(byte))
            elif byte == 0x1B:
                self.stats.malformed_sequences += 1
                self.state = _ESCAPE
                self.sequence.clear()
            elif 0x20 <= byte <= 0x3F:
                if len(self.sequence) >= SEQUENCE_CAPACITY:
                    self.stats.malformed_sequences += 1
                    self.state = _CSI_DISCARD
                    self.sequence.clear()
                else:
                    self.sequence.append(byte)
            else:
                self.stats.malformed_sequences += 1
                self.state = _GROUND
                self.sequence.clear()
        elif state == _CSI_DISCARD:
            if 0x40 <= byte <= 0x7E:
                self.state = _GROUND
            elif byte == 0x1B:
                self.state = _ESCAPE
            elif _cancels(byte):
                self.state = _GROUND
        elif state == _SS3:
            self.ss3(byte)
        elif state == _STRING:
            if byte == 0x1B:
                self.state = _STRING_ESCAPE
            elif byte == 0x9C or _cancels(byte) or (
                    self.string_osc and byte == 0x07):
                self.state = _GROUND
                self.stats.ignored_sequences += 1
        elif state == _STRING_ESCAPE:
            if byte in (ord("\\"), 0x9C) or _cancels(byte):
                self.state = _GROUND
                self.stats.ignored_sequences += 1
            elif self.string_osc and byte == 0x07:
                self.state = _GROUND
                self.stats.ignored_sequences += 1
            elif byte != 0x1B:
                self.state = _STRING


@dataclass
class Input:
    """An incremental decoder. Feed bytes; take events.

    ``pixel_coordinates`` only labels mouse events: the terminal decides the
    unit (mode 1016), the application asked for it, and this records which.

    ``stats`` counts what the outer layer saw (mouse, focus, paste and the
    strings it consumed); ``keyboard_stats`` counts the keyboard layer, as
    ``kittyin_input_get_stats`` and ``kittykb_input_get_stats`` do in C.
    """

    pixel_coordinates: bool = False
    paste_chunk: int = PASTE_CHUNK_CAPACITY
    stats: Stats = field(default_factory=Stats)

    def __post_init__(self) -> None:
        if self.paste_chunk < 1:
            raise ValueError("paste_chunk must be positive")
        self._queue: deque[Event] = deque()
        self._keyboard = _Keyboard()
        self._state = _GROUND
        self._sequence = bytearray()
        self._paste = bytearray()
        self._paste_match = 0

    # ------------------------------------------------------------ public API

    @property
    def keyboard_stats(self) -> Stats:
        return self._keyboard.stats

    def feed(self, data: bytes | bytearray | memoryview) -> None:
        data = bytes(data)
        self.stats.bytes_fed += len(data)
        self._drain()
        index, size = 0, len(data)
        while index < size:
            byte = data[index]
            if self._state == _GROUND and byte != 0x1B:
                end = data.find(b"\x1b", index)
                end = size if end < 0 else end
                self._keyboard.feed(data[index:end])
                index = end
                continue
            if self._state == _PASTE and not self._paste_match \
                    and byte != 0x1B and index + 1 < size:
                end = data.find(b"\x1b", index)
                end = size if end < 0 else end
                for run_byte in data[index:end]:
                    self._paste_byte(run_byte)
                index = end
                continue
            self._byte(byte)
            index += 1
        self._drain()

    def next(self) -> Event | None:
        return self._queue.popleft() if self._queue else None

    def events(self) -> list[Event]:
        """Every queued event, in wire order, and empty the queue."""
        out = list(self._queue)
        self._queue.clear()
        return out

    def __iter__(self) -> Iterator[Event]:
        while self._queue:
            yield self._queue.popleft()

    @property
    def has_pending_escape(self) -> bool:
        """A lone ESC is waiting: either the Escape key or the start of a
        sequence. Call `flush_escape()` once your timeout says which."""
        return self._state == _ESCAPE or self._keyboard.pending_escape

    def flush_escape(self) -> None:
        if self._state == _ESCAPE:
            self._keyboard.feed(b"\x1b")
            self._sequence.clear()
            self._state = _GROUND
        self._keyboard.flush_escape()
        self._drain()

    def reset(self) -> None:
        """Drop partial input and queued events (after a terminal hand-off)."""
        self._queue.clear()
        self._keyboard = _Keyboard()
        self._state = _GROUND
        self._sequence.clear()
        self._paste.clear()
        self._paste_match = 0

    # ------------------------------------------------------------- internals

    def _emit(self, event: Event) -> None:
        self.stats.events_decoded += 1
        self._queue.append(event)

    def _drain(self) -> None:
        if self._keyboard.events:
            for event in self._keyboard.events:
                self._emit(event)
            self._keyboard.events.clear()

    def _start_escape(self) -> None:
        self._sequence = bytearray()
        self._append(0x1B)
        self._state = _ESCAPE

    def _append(self, byte: int) -> bool:
        if len(self._sequence) >= SEQUENCE_CAPACITY:
            self.stats.malformed_sequences += 1
            self._sequence.clear()
            return False
        self._sequence.append(byte)
        return True

    def _finish_string(self) -> None:
        self.stats.ignored_sequences += 1
        self._state = _GROUND

    def _csi(self) -> None:
        sequence = self._sequence.decode("latin-1")
        final = sequence[-1]
        if final in "Mm" and len(sequence) > 2 and sequence[2] == "<":
            event = None
            if len(sequence) >= 7:
                event = _parse_mouse(sequence[2:-1], final,
                                     self.pixel_coordinates)
            if event is None:
                self.stats.malformed_sequences += 1
            else:
                self._drain()
                self._emit(event)
            return
        if sequence == "\x1b[I":
            self._drain()
            self._emit(FocusEvent(True))
            return
        if sequence == "\x1b[O":
            self._drain()
            self._emit(FocusEvent(False))
            return
        if sequence == "\x1b[200~":
            self._paste.clear()
            self._paste_match = 0
            self._state = _PASTE
            return
        if sequence == "\x1b[201~":
            self.stats.ignored_sequences += 1
            return
        self._keyboard_feed(self._sequence)

    def _keyboard_feed(self, data: bytes | bytearray) -> None:
        self._keyboard.feed(data)
        self._drain()

    def _paste_flush(self, final: bool) -> None:
        if not self._paste and not final:
            return
        self._drain()
        self._emit(PasteEvent(bytes(self._paste), final))
        self.stats.paste_chunks += 1
        self._paste.clear()

    def _paste_byte(self, byte: int) -> None:
        if len(self._paste) >= self.paste_chunk:
            self._paste_flush(False)
        self._paste.append(byte)

    def _paste_input(self, byte: int) -> None:
        while True:
            if byte == _PASTE_END[self._paste_match]:
                self._paste_match += 1
                if self._paste_match == len(_PASTE_END):
                    self._paste_match = 0
                    self._paste_flush(True)
                    self._state = _GROUND
                return
            if self._paste_match:
                for prefix_byte in _PASTE_END[: self._paste_match]:
                    self._paste_byte(prefix_byte)
                self._paste_match = 0
                if byte == _PASTE_END[0]:
                    continue
            self._paste_byte(byte)
            return

    def _byte(self, byte: int) -> None:  # noqa: C901 - one state machine
        state = self._state
        if state == _GROUND:
            self._start_escape()  # only ESC reaches here; text is batched
        elif state == _ESCAPE:
            if byte == ord("["):
                self._append(byte)
                self._state = _CSI
            elif byte == ord("O"):
                self._append(byte)
                self._state = _SS3
            elif byte == ord("]"):
                self._sequence.clear()
                self._state = _OSC
            elif byte in b"P^_X":
                self._sequence.clear()
                self._state = _STRING
            elif byte == 0x1B:
                self._keyboard.feed(b"\x1b")
                self._keyboard.flush_escape()
                self._drain()
                self._start_escape()
            else:
                self._append(byte)
                self._keyboard_feed(self._sequence)
                self._sequence.clear()
                self._state = _GROUND
        elif state == _CSI:
            if byte == 0x1B:
                self.stats.malformed_sequences += 1
                self._start_escape()
            elif _cancels(byte):
                self.stats.malformed_sequences += 1
                self._sequence.clear()
                self._state = _GROUND
            elif not self._append(byte):
                self._state = _CSI_DISCARD
            elif 0x40 <= byte <= 0x7E:
                self._state = _GROUND
                self._csi()
                if self._state != _PASTE:
                    self._sequence.clear()
        elif state == _SS3:
            if byte == 0x1B:
                self.stats.malformed_sequences += 1
                self._start_escape()
            elif _cancels(byte):
                self.stats.malformed_sequences += 1
                self._sequence.clear()
                self._state = _GROUND
            elif not self._append(byte):
                self._state = _SS3_DISCARD
            elif 0x30 <= byte <= 0x7E:
                self._keyboard_feed(self._sequence)
                self._sequence.clear()
                self._state = _GROUND
        elif state == _OSC:
            if byte in (0x07, 0x9C) or _cancels(byte):
                self._finish_string()
            elif byte == 0x1B:
                self._state = _OSC_ESCAPE
        elif state == _OSC_ESCAPE:
            if byte in (ord("\\"), 0x9C, 0x07) or _cancels(byte):
                self._finish_string()
            elif byte != 0x1B:
                self._state = _OSC
        elif state == _STRING:
            if byte == 0x9C or _cancels(byte):
                self._finish_string()
            elif byte == 0x1B:
                self._state = _STRING_ESCAPE
        elif state == _STRING_ESCAPE:
            if byte in (ord("\\"), 0x9C) or _cancels(byte):
                self._finish_string()
            elif byte != 0x1B:
                self._state = _STRING
        elif state == _CSI_DISCARD:
            if byte == 0x1B:
                self._start_escape()
            elif 0x40 <= byte <= 0x7E or _cancels(byte):
                self._state = _GROUND
        elif state == _SS3_DISCARD:
            if byte == 0x1B:
                self._start_escape()
            elif 0x30 <= byte <= 0x7E or _cancels(byte):
                self._state = _GROUND
        elif state == _PASTE:
            self._paste_input(byte)


def decode(data: bytes, *, flush: bool = True, **options: object) -> list[Event]:
    """Decode a complete buffer. A trailing lone ESC is the Escape key unless
    ``flush=False``."""
    decoder = Input(**options)  # type: ignore[arg-type]
    decoder.feed(data)
    if flush:
        decoder.flush_escape()
    return decoder.events()
