"""The mode switches that make a terminal send what the decoder reads.

These are strings, not actions: the decoder never writes to a terminal, and
an application that owns its terminal decides when to send them. Send the
``*_ON`` strings on entry and the matching ``*_OFF`` strings, in reverse
order, on every exit path.
"""

#: Button-event tracking (1000 + 1002), any-motion (1003) so hover works, and
#: the SGR encoding (1006), the only one that survives past column 223.
MOUSE_ON = "\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h"
MOUSE_OFF = "\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l"

#: Bracketed paste (2004): pasted text arrives framed, never as keystrokes.
PASTE_ON = "\x1b[?2004h"
PASTE_OFF = "\x1b[?2004l"

#: Focus reporting (1004).
FOCUS_ON = "\x1b[?1004h"
FOCUS_OFF = "\x1b[?1004l"

#: Kitty keyboard protocol, progressive enhancement flag 1 (disambiguate
#: escape codes), pushed on the terminal's stack so it is restored exactly.
#: A terminal without the protocol ignores both.
KEYBOARD_PUSH_DISAMBIGUATE = "\x1b[>1u"
KEYBOARD_POP = "\x1b[<u"
