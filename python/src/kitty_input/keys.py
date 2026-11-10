"""Key codes and modifier bits, numerically identical to the C headers.

`kitty_keyboard.h` assigns every non-text key a value in the private-use
range starting at U+E000, and a text key is its own Unicode scalar. The
values here are copied from that header so an event decoded in Python and one
decoded in C compare equal field for field.
"""

from __future__ import annotations

# Modifier bits (KITTYKB_MOD_*).
SHIFT = 1 << 0
ALT = 1 << 1
CTRL = 1 << 2
SUPER = 1 << 3
HYPER = 1 << 4
META = 1 << 5
CAPS_LOCK_MOD = 1 << 6
NUM_LOCK_MOD = 1 << 7

#: The modifiers a binding should compare. Lock state is not a chord.
CHORD_MODIFIERS = SHIFT | ALT | CTRL | SUPER | HYPER | META

# Key actions (KITTYKB_ACTION_*).
PRESS = 1
REPEAT = 2
RELEASE = 3

# Event sources (KITTYKB_SOURCE_*).
SOURCE_LEGACY = 0
SOURCE_KITTY = 1

NONE = 0
ESCAPE = 0xE000
ENTER = 0xE001
TAB = 0xE002
BACKSPACE = 0xE003
INSERT = 0xE004
DELETE = 0xE005
LEFT = 0xE006
RIGHT = 0xE007
UP = 0xE008
DOWN = 0xE009
PAGE_UP = 0xE00A
PAGE_DOWN = 0xE00B
HOME = 0xE00C
END = 0xE00D
CAPS_LOCK = 0xE00E
SCROLL_LOCK = 0xE00F
NUM_LOCK = 0xE010
PRINT_SCREEN = 0xE011
PAUSE = 0xE012
MENU = 0xE013
F1 = 0xE014
#: F1..F35 are consecutive: ``F1 + n - 1``.
F35 = 0xE036
KP_0 = 0xE037
KP_9 = 0xE040
KP_DECIMAL = 0xE041
KP_DIVIDE = 0xE042
KP_MULTIPLY = 0xE043
KP_SUBTRACT = 0xE044
KP_ADD = 0xE045
KP_ENTER = 0xE046
KP_EQUAL = 0xE047
KP_SEPARATOR = 0xE048
KP_LEFT = 0xE049
KP_RIGHT = 0xE04A
KP_UP = 0xE04B
KP_DOWN = 0xE04C
KP_PAGE_UP = 0xE04D
KP_PAGE_DOWN = 0xE04E
KP_HOME = 0xE04F
KP_END = 0xE050
KP_INSERT = 0xE051
KP_DELETE = 0xE052
KP_BEGIN = 0xE053


def F(number: int) -> int:  # noqa: N802 - mirrors the C naming
    """The code for function key F`number`, 1 through 35."""
    if not 1 <= number <= 35:
        raise ValueError("function keys run from F1 to F35")
    return F1 + number - 1


_NAMES = {
    ESCAPE: "escape", ENTER: "enter", TAB: "tab", BACKSPACE: "backspace",
    INSERT: "insert", DELETE: "delete", LEFT: "left", RIGHT: "right",
    UP: "up", DOWN: "down", PAGE_UP: "page_up", PAGE_DOWN: "page_down",
    HOME: "home", END: "end", CAPS_LOCK: "caps_lock",
    SCROLL_LOCK: "scroll_lock", NUM_LOCK: "num_lock",
    PRINT_SCREEN: "print_screen", PAUSE: "pause", MENU: "menu",
    KP_DECIMAL: "kp_decimal", KP_DIVIDE: "kp_divide",
    KP_MULTIPLY: "kp_multiply", KP_SUBTRACT: "kp_subtract",
    KP_ADD: "kp_add", KP_ENTER: "kp_enter", KP_EQUAL: "kp_equal",
    KP_SEPARATOR: "kp_separator", KP_LEFT: "kp_left", KP_RIGHT: "kp_right",
    KP_UP: "kp_up", KP_DOWN: "kp_down", KP_PAGE_UP: "kp_page_up",
    KP_PAGE_DOWN: "kp_page_down", KP_HOME: "kp_home", KP_END: "kp_end",
    KP_INSERT: "kp_insert", KP_DELETE: "kp_delete", KP_BEGIN: "kp_begin",
}
for _n in range(1, 36):
    _NAMES[F(_n)] = f"f{_n}"
for _n in range(10):
    _NAMES[KP_0 + _n] = f"kp_{_n}"
_CODES = {name: code for code, name in _NAMES.items()}
_CODES.update({"esc": ESCAPE, "return": ENTER, "space": ord(" "),
               "pgup": PAGE_UP, "pgdn": PAGE_DOWN, "del": DELETE,
               "ins": INSERT})

_MODIFIER_NAMES = (
    (CTRL, "ctrl"), (ALT, "alt"), (SHIFT, "shift"),
    (SUPER, "super"), (HYPER, "hyper"), (META, "meta"),
)
_MODIFIER_CODES = {name: bit for bit, name in _MODIFIER_NAMES}
_MODIFIER_CODES.update({"control": CTRL, "opt": ALT, "option": ALT,
                        "cmd": SUPER})


def key_name(key: int) -> str:
    """A stable lower-case name: ``up``, ``f5``, ``a``, ``space``, ``é``."""
    if key in _NAMES:
        return _NAMES[key]
    if key == 0x20:
        return "space"
    if 0 < key < 0x20:
        return f"c0_{key:02x}"
    try:
        return chr(key)
    except (ValueError, OverflowError):
        return f"u{key:x}"


def chord_name(key: int, modifiers: int) -> str:
    """``ctrl+shift+up``: modifiers in a fixed order, then the key."""
    parts = [name for bit, name in _MODIFIER_NAMES if modifiers & bit]
    parts.append(key_name(key))
    return "+".join(parts)


def parse_chord(text: str) -> tuple[int, int]:
    """The inverse of `chord_name`: ``"ctrl+s"`` -> ``(ord('s'), CTRL)``.

    A single printable character is its own key, so ``"+"`` and ``"ctrl++"``
    both work. Names are case-insensitive; a lone upper-case letter is kept
    as typed, because it is the text the terminal sends for shift+letter.
    """
    if not text:
        raise ValueError("empty key chord")
    if len(text) == 1:
        return ord(text), 0
    if text.endswith("++"):
        head, last = text[:-2], "+"
    else:
        head, _, last = text.rpartition("+")
    modifiers = 0
    for part in filter(None, head.split("+")):
        try:
            modifiers |= _MODIFIER_CODES[part.lower()]
        except KeyError:
            raise ValueError(f"unknown modifier {part!r} in {text!r}") from None
    if len(last) == 1:
        key = ord(last.lower()) if modifiers & CTRL else ord(last)
    else:
        try:
            key = _CODES[last.lower()]
        except KeyError:
            raise ValueError(f"unknown key {last!r} in {text!r}") from None
    return key, modifiers
