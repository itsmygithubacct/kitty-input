"""kitty-input for Python: terminal keyboard, mouse, focus and paste decoding.

A standard-library port of the C parser in this repository, with the same
event model and the same key codes. See `decoder` for the contract.

    from kitty_input import Input, KeyEvent, MouseEvent, keys

    decoder = Input()
    decoder.feed(os.read(fd, 4096))
    for event in decoder:
        if isinstance(event, KeyEvent) and event.chord == "ctrl+s":
            save()
"""

from . import keys
from .decoder import (
    MOUSE_MOVE,
    MOUSE_PRESS,
    MOUSE_RELEASE,
    MOUSE_WHEEL,
    Event,
    FocusEvent,
    Input,
    KeyEvent,
    MouseEvent,
    PasteEvent,
    Stats,
    decode,
)
from .terminal import (
    FOCUS_OFF,
    FOCUS_ON,
    KEYBOARD_POP,
    KEYBOARD_PUSH_DISAMBIGUATE,
    MOUSE_OFF,
    MOUSE_ON,
    PASTE_OFF,
    PASTE_ON,
)

__version__ = "0.2.1"

__all__ = [
    "Event",
    "FOCUS_OFF",
    "FOCUS_ON",
    "FocusEvent",
    "Input",
    "KEYBOARD_POP",
    "KEYBOARD_PUSH_DISAMBIGUATE",
    "KeyEvent",
    "MOUSE_MOVE",
    "MOUSE_OFF",
    "MOUSE_ON",
    "MOUSE_PRESS",
    "MOUSE_RELEASE",
    "MOUSE_WHEEL",
    "MouseEvent",
    "PASTE_OFF",
    "PASTE_ON",
    "PasteEvent",
    "Stats",
    "decode",
    "keys",
]
