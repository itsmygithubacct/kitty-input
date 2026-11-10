"""The Python decoder against the wire the C suite (tests/test_input.c) uses."""

from __future__ import annotations

import unittest

from kitty_input import (
    MOUSE_MOVE,
    MOUSE_PRESS,
    MOUSE_RELEASE,
    MOUSE_WHEEL,
    FocusEvent,
    Input,
    KeyEvent,
    MouseEvent,
    PasteEvent,
    decode,
    keys as K,
)

C_SUITE_WIRE = bytes(
    [ord("a"), 0xC3, 0xA9]
    + list(b"\x1b[<0;2;3M")
    + list(b"\x1b]osc") + [0x9C]
    + list(b"\x1bPdcs") + [0x07] + list(b"x\x1b\\")
    + list(b"\x1b[200~")
    + list(b"paste\x1b[20x")
    + list(b"\x1b[201~")
    + list(b"\x1b[113u")
)


def feed_bytewise(data: bytes, **options) -> Input:
    decoder = Input(**options)
    for byte in data:
        decoder.feed(bytes([byte]))
    return decoder


class WireOrder(unittest.TestCase):
    def test_mixed_stream_keeps_order(self) -> None:
        pasted = b"multi\nline"
        wire = (
            b"\x1b[97u"            # a press
            b"\x1b[<0;11;7M"       # mouse press at (10, 6)
            b"\x1b[97;1:3u"        # a release
            b"\x1b[I"
            b"\x1b[200~" + pasted + b"\x1b[201~"
            b"\x1b[O"
        )
        decoder = feed_bytewise(wire, pixel_coordinates=True)
        events = decoder.events()
        self.assertEqual([e.kind for e in events],
                         ["key", "mouse", "key", "focus", "paste", "focus"])
        self.assertEqual(events[0].key, ord("a"))
        self.assertEqual(events[0].action, K.PRESS)
        self.assertEqual((events[1].x, events[1].y), (10, 6))
        self.assertEqual(events[1].button, 1)
        self.assertTrue(events[1].pixel_coordinates)
        self.assertEqual(events[2].action, K.RELEASE)
        self.assertTrue(events[2].event_type_explicit)
        self.assertTrue(events[3].focused)
        self.assertEqual(events[4].data, pasted)
        self.assertTrue(events[4].final)
        self.assertFalse(events[5].focused)
        self.assertEqual(decoder.stats.malformed_sequences, 0)

    def test_bulk_and_fragmented_are_identical(self) -> None:
        bulk = Input()
        bulk.feed(C_SUITE_WIRE)
        fragmented = feed_bytewise(C_SUITE_WIRE)
        self.assertEqual(bulk.events(), fragmented.events())
        self.assertEqual(bulk.stats, fragmented.stats)

    def test_c_suite_wire_decodes_as_expected(self) -> None:
        events = decode(C_SUITE_WIRE)
        self.assertEqual(events[0], KeyEvent(key=ord("a"), text="a"))
        self.assertEqual(events[1], KeyEvent(key=0xE9, text="é"))
        self.assertIsInstance(events[2], MouseEvent)
        self.assertEqual(events[3], PasteEvent(b"paste\x1b[20x", True))
        self.assertEqual(events[4].key, ord("q"))
        self.assertEqual(events[4].source, K.SOURCE_KITTY)
        self.assertEqual(len(events), 5)


class Mouse(unittest.TestCase):
    def test_matrix(self) -> None:
        events = decode(
            b"\x1b[<20;3;4M"   # ctrl + shift + button 1
            b"\x1b[<32;5;6M"   # button 1 motion
            b"\x1b[<0;5;6m"    # button 1 release
            b"\x1b[<64;7;8M"   # wheel up
            b"\x1b[<65;7;8M"   # wheel down
            b"\x1b[<66;7;8M"   # wheel left
            b"\x1b[<67;7;8M"   # wheel right
        )
        self.assertEqual(events[0].action, MOUSE_PRESS)
        self.assertEqual(events[0].modifiers, K.SHIFT | K.CTRL)
        self.assertFalse(events[0].pixel_coordinates)
        self.assertEqual((events[1].action, events[1].button), (MOUSE_MOVE, 1))
        self.assertEqual((events[2].action, events[2].button), (MOUSE_RELEASE, 1))
        wheels = [(e.action, e.wheel_x, e.wheel_y) for e in events[3:]]
        self.assertEqual(wheels, [
            (MOUSE_WHEEL, 0, -1), (MOUSE_WHEEL, 0, 1),
            (MOUSE_WHEEL, -1, 0), (MOUSE_WHEEL, 1, 0),
        ])

    def test_hover_without_button_is_motion_with_no_button(self) -> None:
        (event,) = decode(b"\x1b[<35;10;4M")
        self.assertEqual((event.action, event.button), (MOUSE_MOVE, 0))
        self.assertEqual((event.x, event.y), (9, 3))

    def test_extra_buttons(self) -> None:
        (event,) = decode(b"\x1b[<129;1;1M")
        self.assertEqual(event.button, 9)

    def test_zero_coordinate_is_malformed(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b[<0;0;1Mq")
        self.assertEqual([e.key for e in decoder.events()], [ord("q")])
        self.assertEqual(decoder.stats.malformed_sequences, 1)


class Paste(unittest.TestCase):
    def test_chunking_and_failed_terminator_prefix(self) -> None:
        long_paste = bytes(ord("a") + i % 26 for i in range(128 + 9))
        decoder = Input()
        for byte in b"\x1b[200~":
            decoder.feed(bytes([byte]))
        decoder.feed(long_paste)
        for byte in b"\x1b[20x" + b"\x1b[201~":
            decoder.feed(bytes([byte]))
        first, second = decoder.events()
        self.assertFalse(first.final)
        self.assertEqual(first.data, long_paste[:128])
        self.assertTrue(second.final)
        self.assertEqual(second.data, long_paste[128:] + b"\x1b[20x")
        self.assertEqual(decoder.stats.paste_chunks, 2)

    def test_paste_is_never_keystrokes(self) -> None:
        events = decode(b"\x1b[200~\x1b[A\r\x03\x1b[201~")
        self.assertEqual(events, [PasteEvent(b"\x1b[A\r\x03", True)])
        self.assertEqual(events[0].text, "\x1b[A\r\x03")

    def test_empty_paste_still_reports_final(self) -> None:
        self.assertEqual(decode(b"\x1b[200~\x1b[201~"), [PasteEvent(b"", True)])


class FilteringAndRecovery(unittest.TestCase):
    def test_strings_are_consumed(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b]title\x07\x1bPignored\x1b\\\x1b[<;1;2Mq")
        self.assertEqual([e.key for e in decoder.events()], [ord("q")])
        self.assertEqual(decoder.stats.ignored_sequences, 2)
        self.assertEqual(decoder.stats.malformed_sequences, 1)

    def test_overlong_csi_is_discarded_then_resyncs(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b[" + b"1" * (256 + 12) + b"M")
        decoder.feed(b"r")
        self.assertEqual([e.key for e in decoder.events()], [ord("r")])
        self.assertEqual(decoder.stats.malformed_sequences, 1)

    def test_fresh_escape_resyncs_from_discard(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b[" + b"1" * (256 + 8) + b"\x1b[113u")
        self.assertEqual([e.key for e in decoder.events()], [ord("q")])

    def test_control_string_isolation(self) -> None:
        wire = bytes(
            [0x1B] + list(b"Pdcs") + [0x07] + list(b"not-text") + [0x1B, ord("\\")]
            + [0x1B] + list(b"]osc") + [0x9C]
            + [0x1B] + list(b"]osc") + [0x1B, 0x07]
            + [0x1B] + list(b"Xsos") + [0x07] + list(b"still-hidden")
            + [0x1B, ord("\\")]
            + [0x1B] + list(b"Pcan") + [0x18] + [ord("r")]
            + [0x1B] + list(b"]sub") + [0x1B, 0x1A] + [ord("s")]
            + [ord("q")]
        )
        decoder = Input()
        decoder.feed(wire)
        self.assertEqual([e.key for e in decoder.events()],
                         [ord("r"), ord("s"), ord("q")])
        self.assertEqual(decoder.stats.ignored_sequences, 6)

    def test_terminal_replies_are_not_keys(self) -> None:
        self.assertEqual(decode(b"\x1b[?1u\x1b[?62;22c"), [])


class Keyboard(unittest.TestCase):
    def test_legacy_named_keys(self) -> None:
        table = {
            b"\x1b[A": K.UP, b"\x1b[B": K.DOWN, b"\x1b[C": K.RIGHT,
            b"\x1b[D": K.LEFT, b"\x1b[H": K.HOME, b"\x1b[F": K.END,
            b"\x1bOA": K.UP, b"\x1bOP": K.F1, b"\x1bOR": K.F(3),
            b"\x1b[2~": K.INSERT, b"\x1b[3~": K.DELETE,
            b"\x1b[5~": K.PAGE_UP, b"\x1b[6~": K.PAGE_DOWN,
            b"\x1b[1~": K.F1 if False else None,
            b"\x1b[15~": K.F(5), b"\x1b[24~": K.F(12),
            b"\r": K.ENTER, b"\t": K.TAB, b"\x7f": K.BACKSPACE,
            b"\x08": K.BACKSPACE,
        }
        for wire, expected in table.items():
            if expected is None:
                continue
            with self.subTest(wire=wire):
                (event,) = decode(wire)
                self.assertEqual(event.key, expected)

    def test_unknown_tilde_is_malformed(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b[1~")
        self.assertEqual(decoder.events(), [])
        self.assertEqual(decoder.keyboard_stats.malformed_sequences, 1)

    def test_legacy_modifiers(self) -> None:
        (event,) = decode(b"\x1b[1;5A")
        self.assertEqual((event.key, event.modifiers), (K.UP, K.CTRL))
        (event,) = decode(b"\x1b[3;2~")
        self.assertEqual((event.key, event.modifiers), (K.DELETE, K.SHIFT))
        (event,) = decode(b"\x1b[Z")
        self.assertEqual((event.key, event.modifiers), (K.TAB, K.SHIFT))

    def test_alt_prefix(self) -> None:
        (event,) = decode(b"\x1bx")
        self.assertEqual((event.key, event.modifiers, event.text),
                         (ord("x"), K.ALT, "x"))
        (event,) = decode(b"\x1b\xc3\xa9")
        self.assertEqual((event.key, event.modifiers), (0xE9, K.ALT))

    def test_lone_escape_waits_for_flush(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b")
        self.assertTrue(decoder.has_pending_escape)
        self.assertEqual(decoder.events(), [])
        decoder.flush_escape()
        self.assertEqual(decoder.events(), [KeyEvent(key=K.ESCAPE)])
        self.assertFalse(decoder.has_pending_escape)

    def test_double_escape(self) -> None:
        decoder = Input()
        decoder.feed(b"\x1b\x1b")
        self.assertEqual([e.key for e in decoder.events()], [K.ESCAPE])
        self.assertTrue(decoder.has_pending_escape)

    def test_kitty_protocol(self) -> None:
        (event,) = decode(b"\x1b[97:65;2;65u")
        self.assertEqual(event.key, ord("a"))
        self.assertEqual(event.shifted_key, ord("A"))
        self.assertEqual(event.modifiers, K.SHIFT)
        self.assertEqual(event.text, "A")
        (event,) = decode(b"\x1b[27u")
        self.assertEqual(event.key, K.ESCAPE)
        (event,) = decode(b"\x1b[13;5u")
        self.assertEqual((event.key, event.modifiers), (K.ENTER, K.CTRL))
        (event,) = decode(b"\x1b[1;5:2A")
        self.assertEqual((event.key, event.action), (K.UP, K.REPEAT))

    def test_kitty_rejects_bad_fields(self) -> None:
        for wire in (b"\x1b[97;0u", b"\x1b[97;1:4u", b"\x1b[97;1;31u",
                     b"\x1b[1114112u", b"\x1b[97;1;97;1u"):
            with self.subTest(wire=wire):
                decoder = Input()
                decoder.feed(wire)
                self.assertEqual(decoder.events(), [])
                self.assertEqual(decoder.keyboard_stats.malformed_sequences, 1)

    def test_utf8_and_malformed_utf8(self) -> None:
        events = decode("日本".encode())
        self.assertEqual("".join(e.text for e in events), "日本")
        decoder = Input()
        decoder.feed(b"\xc3x\xffy\xe0\x80\x80")
        self.assertEqual([e.text for e in decoder.events()], ["x", "y"])
        self.assertEqual(decoder.keyboard_stats.malformed_sequences, 3)

    def test_escape_interrupts_partial_utf8(self) -> None:
        decoder = Input()
        decoder.feed(b"\xe6\x97\x1b[A")
        self.assertEqual([e.key for e in decoder.events()], [K.UP])
        self.assertEqual(decoder.keyboard_stats.malformed_sequences, 1)


class Normalisation(unittest.TestCase):
    def test_c0_letters_become_ctrl_chords(self) -> None:
        (event,) = decode(b"\x13")
        self.assertEqual(event.key, 0x13)          # wire-faithful
        self.assertEqual(event.chord, "ctrl+s")    # normalised
        (kitty,) = decode(b"\x1b[115;5u")
        self.assertEqual(kitty.chord, "ctrl+s")

    def test_named_chords(self) -> None:
        (event,) = decode(b"\x1b[1;6B")
        self.assertEqual(event.chord, "ctrl+shift+down")
        (event,) = decode(b" ")
        self.assertEqual(event.chord, "space")
        self.assertEqual(decode(b"\x00")[0].chord, "ctrl+space")

    def test_parse_chord_round_trips(self) -> None:
        for text in ("ctrl+s", "ctrl+shift+down", "f5", "enter", "+",
                     "ctrl++", "alt+x", "space", "escape"):
            with self.subTest(text=text):
                key, mods = K.parse_chord(text)
                self.assertEqual(K.parse_chord(K.chord_name(key, mods)),
                                 (key, mods))
        self.assertEqual(K.parse_chord("Ctrl+S"), (ord("s"), K.CTRL))
        with self.assertRaises(ValueError):
            K.parse_chord("hyperdrive+x")


class Fuzz(unittest.TestCase):
    def test_random_bytes_never_raise_and_split_invariance_holds(self) -> None:
        import random

        rng = random.Random(1234)
        alphabet = b"\x1b[]<;:0123456789MmuABCDHFPQSZ~O\x07\x9c\x18\x1a\\Ia\xc3\xa9\xe6\x97\xa5"
        for _ in range(300):
            wire = bytes(rng.choice(alphabet) for _ in range(rng.randint(0, 80)))
            bulk = Input()
            bulk.feed(wire)
            split = Input()
            cut = rng.randint(0, len(wire))
            split.feed(wire[:cut])
            split.feed(wire[cut:])
            self.assertEqual(bulk.events(), split.events(), wire)


if __name__ == "__main__":
    unittest.main()
