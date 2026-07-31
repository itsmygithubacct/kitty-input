#include "kitty_keyboard.h"

#include <limits.h>
#include <string.h>

enum parser_state {
    PARSER_GROUND = 0,
    PARSER_ESCAPE,
    PARSER_CSI,
    PARSER_CSI_DISCARD,
    PARSER_SS3,
    PARSER_STRING,
    PARSER_STRING_ESCAPE
};

enum string_kind {
    STRING_OTHER = 0,
    STRING_OSC = 1
};

static bool
is_unicode_scalar(uint32_t value)
{
    return value <= 0x10ffffu &&
           !(value >= 0xd800u && value <= 0xdfffu);
}

static uint32_t
canonical_key(uint32_t key)
{
    switch (key) {
    case 27u:
        return KITTYKB_KEY_ESCAPE;
    case 13u:
        return KITTYKB_KEY_ENTER;
    case 9u:
        return KITTYKB_KEY_TAB;
    case 127u:
        return KITTYKB_KEY_BACKSPACE;
    default:
        return key;
    }
}

static bool
parse_decimal(
    const char *text,
    size_t begin,
    size_t end,
    uint32_t *value)
{
    uint32_t result = 0u;
    size_t index;

    if (text == NULL || value == NULL || begin >= end) {
        return false;
    }
    for (index = begin; index < end; ++index) {
        const unsigned char character = (unsigned char)text[index];
        const uint32_t digit = (uint32_t)(character - (unsigned char)'0');

        if (character < (unsigned char)'0' ||
            character > (unsigned char)'9' ||
            result > (UINT32_MAX - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }
    *value = result;
    return true;
}

static bool
held_entry_matches_event(
    const kittykb_held_key *held,
    const kittykb_event *event)
{
    if (held->key == event->key) {
        return true;
    }
    return held->base_layout_key != 0u &&
           event->base_layout_key != 0u &&
           held->base_layout_key == event->base_layout_key;
}

static void
update_held_state(kittykb_input *input, const kittykb_event *event)
{
    size_t index;
    const bool flags_allow_state =
        (input->terminal_flags &
         (KITTYKB_FLAG_REPORT_EVENTS | KITTYKB_FLAG_REPORT_ALL_KEYS)) ==
        (KITTYKB_FLAG_REPORT_EVENTS | KITTYKB_FLAG_REPORT_ALL_KEYS);

    if (!flags_allow_state &&
        !(input->support == (uint8_t)KITTYKB_SUPPORT_UNKNOWN &&
          event->event_type_explicit)) {
        return;
    }
    if (event->key == KITTYKB_KEY_NONE) {
        return;
    }

    for (index = 0u; index < input->held_count; ++index) {
        if (held_entry_matches_event(&input->held[index], event)) {
            break;
        }
    }

    if (event->action == KITTYKB_ACTION_RELEASE) {
        input->observed_explicit_release = true;
        if (index < input->held_count) {
            --input->held_count;
            input->held[index] = input->held[input->held_count];
        }
        return;
    }

    if (index < input->held_count) {
        input->held[index].key = event->key;
        input->held[index].shifted_key = event->shifted_key;
        input->held[index].base_layout_key = event->base_layout_key;
        input->held[index].modifiers = event->modifiers;
        return;
    }
    if (input->held_count >= KITTYKB_HELD_KEY_CAPACITY) {
        ++input->stats.dropped_held_keys;
        return;
    }

    input->held[input->held_count].key = event->key;
    input->held[input->held_count].shifted_key = event->shifted_key;
    input->held[input->held_count].base_layout_key =
        event->base_layout_key;
    input->held[input->held_count].modifiers = event->modifiers;
    ++input->held_count;
}

static void
emit_event(kittykb_input *input, const kittykb_event *event)
{
    size_t tail;

    update_held_state(input, event);
    ++input->stats.events_decoded;
    if (input->queue_count >= KITTYKB_EVENT_QUEUE_CAPACITY) {
        ++input->stats.dropped_events;
        return;
    }
    tail = (input->queue_head + input->queue_count) %
           KITTYKB_EVENT_QUEUE_CAPACITY;
    input->queue[tail] = *event;
    ++input->queue_count;
}

static void
emit_simple(
    kittykb_input *input,
    uint32_t key,
    uint32_t modifiers,
    kittykb_action action,
    kittykb_event_source source,
    bool explicit_action)
{
    kittykb_event event;

    (void)memset(&event, 0, sizeof(event));
    event.key = key;
    event.modifiers = modifiers;
    event.action = action;
    event.source = (uint8_t)source;
    event.event_type_explicit = explicit_action;
    emit_event(input, &event);
}

static bool
parse_modifier_action(
    const char *text,
    size_t begin,
    size_t end,
    uint32_t *modifiers,
    kittykb_action *action,
    bool *explicit_action)
{
    size_t colon = end;
    size_t index;
    uint32_t encoded_modifiers = 1u;
    uint32_t encoded_action = 1u;

    *modifiers = 0u;
    *action = KITTYKB_ACTION_PRESS;
    *explicit_action = false;
    if (begin == end) {
        return true;
    }
    for (index = begin; index < end; ++index) {
        if (text[index] == ':') {
            if (colon != end) {
                return false;
            }
            colon = index;
        }
    }
    if (!parse_decimal(text, begin, colon, &encoded_modifiers) ||
        encoded_modifiers == 0u || encoded_modifiers > 256u) {
        return false;
    }
    if (colon < end) {
        if (!parse_decimal(text, colon + 1u, end, &encoded_action) ||
            encoded_action < 1u || encoded_action > 3u) {
            return false;
        }
        *explicit_action = true;
    }
    *modifiers = encoded_modifiers - 1u;
    *action = (kittykb_action)encoded_action;
    return true;
}

static bool
parse_key_field(
    const char *text,
    size_t begin,
    size_t end,
    kittykb_event *event)
{
    size_t first_colon = end;
    size_t second_colon = end;
    size_t index;
    uint32_t key;

    for (index = begin; index < end; ++index) {
        if (text[index] == ':') {
            if (first_colon == end) {
                first_colon = index;
            } else if (second_colon == end) {
                second_colon = index;
            } else {
                return false;
            }
        }
    }
    if (!parse_decimal(text, begin, first_colon, &key) ||
        !is_unicode_scalar(key)) {
        return false;
    }
    event->key = canonical_key(key);
    event->shifted_key = 0u;
    event->base_layout_key = 0u;

    if (first_colon < end) {
        const size_t shifted_end =
            second_colon < end ? second_colon : end;
        if (first_colon + 1u < shifted_end &&
            (!parse_decimal(
                 text,
                 first_colon + 1u,
                 shifted_end,
                 &event->shifted_key) ||
             !is_unicode_scalar(event->shifted_key))) {
            return false;
        }
    }
    if (second_colon < end && second_colon + 1u < end &&
        (!parse_decimal(
             text,
             second_colon + 1u,
             end,
             &event->base_layout_key) ||
         !is_unicode_scalar(event->base_layout_key))) {
        return false;
    }
    return true;
}

static bool
is_allowed_text_codepoint(uint32_t value)
{
    return is_unicode_scalar(value) &&
           value >= 0x20u &&
           !(value >= 0x7fu && value <= 0x9fu);
}

static bool
parse_text_field(
    const char *text,
    size_t begin,
    size_t end,
    kittykb_event *event)
{
    size_t field_begin = begin;
    size_t index;

    event->text_length = 0u;
    if (begin == end) {
        return true;
    }
    for (index = begin; index <= end; ++index) {
        if (index == end || text[index] == ':') {
            uint32_t value;

            if (event->text_length >= KITTYKB_MAX_TEXT_CODEPOINTS ||
                !parse_decimal(text, field_begin, index, &value) ||
                !is_allowed_text_codepoint(value)) {
                return false;
            }
            event->text[event->text_length] = value;
            ++event->text_length;
            field_begin = index + 1u;
        }
    }
    return true;
}

static bool
parse_kitty_key(
    const char *text,
    size_t size,
    kittykb_event *event)
{
    size_t first_semicolon = size;
    size_t second_semicolon = size;
    size_t index;

    (void)memset(event, 0, sizeof(*event));
    event->action = KITTYKB_ACTION_PRESS;
    event->source = (uint8_t)KITTYKB_SOURCE_KITTY;

    for (index = 0u; index < size; ++index) {
        if (text[index] == ';') {
            if (first_semicolon == size) {
                first_semicolon = index;
            } else if (second_semicolon == size) {
                second_semicolon = index;
            } else {
                return false;
            }
        }
    }
    if (!parse_key_field(text, 0u, first_semicolon, event)) {
        return false;
    }
    if (first_semicolon < size &&
        !parse_modifier_action(
            text,
            first_semicolon + 1u,
            second_semicolon,
            &event->modifiers,
            &event->action,
            &event->event_type_explicit)) {
        return false;
    }
    if (second_semicolon < size &&
        !parse_text_field(text, second_semicolon + 1u, size, event)) {
        return false;
    }
    return event->key != KITTYKB_KEY_NONE || event->text_length != 0u;
}

static uint32_t
legacy_letter_key(char final)
{
    switch (final) {
    case 'A':
        return KITTYKB_KEY_UP;
    case 'B':
        return KITTYKB_KEY_DOWN;
    case 'C':
        return KITTYKB_KEY_RIGHT;
    case 'D':
        return KITTYKB_KEY_LEFT;
    case 'E':
        return KITTYKB_KEY_KP_BEGIN;
    case 'F':
        return KITTYKB_KEY_END;
    case 'H':
        return KITTYKB_KEY_HOME;
    case 'P':
        return KITTYKB_KEY_F1;
    case 'Q':
        return KITTYKB_KEY_F2;
    case 'S':
        return KITTYKB_KEY_F4;
    case 'Z':
        return KITTYKB_KEY_TAB;
    default:
        return KITTYKB_KEY_NONE;
    }
}

static uint32_t
legacy_tilde_key(uint32_t number)
{
    switch (number) {
    case 2u:
        return KITTYKB_KEY_INSERT;
    case 3u:
        return KITTYKB_KEY_DELETE;
    case 5u:
        return KITTYKB_KEY_PAGE_UP;
    case 6u:
        return KITTYKB_KEY_PAGE_DOWN;
    case 7u:
        return KITTYKB_KEY_HOME;
    case 8u:
        return KITTYKB_KEY_END;
    case 11u:
        return KITTYKB_KEY_F1;
    case 12u:
        return KITTYKB_KEY_F2;
    case 13u:
        return KITTYKB_KEY_F3;
    case 14u:
        return KITTYKB_KEY_F4;
    case 15u:
        return KITTYKB_KEY_F5;
    case 17u:
        return KITTYKB_KEY_F6;
    case 18u:
        return KITTYKB_KEY_F7;
    case 19u:
        return KITTYKB_KEY_F8;
    case 20u:
        return KITTYKB_KEY_F9;
    case 21u:
        return KITTYKB_KEY_F10;
    case 23u:
        return KITTYKB_KEY_F11;
    case 24u:
        return KITTYKB_KEY_F12;
    case 29u:
        return KITTYKB_KEY_MENU;
    default:
        return KITTYKB_KEY_NONE;
    }
}

static bool
parse_legacy_key(
    const char *text,
    size_t size,
    char final,
    kittykb_event *event)
{
    size_t semicolon = size;
    size_t index;
    uint32_t number = 1u;

    (void)memset(event, 0, sizeof(*event));
    event->action = KITTYKB_ACTION_PRESS;
    event->source = (uint8_t)KITTYKB_SOURCE_LEGACY;

    for (index = 0u; index < size; ++index) {
        if (text[index] == ';') {
            if (semicolon != size) {
                return false;
            }
            semicolon = index;
        }
    }
    if (semicolon != 0u && semicolon <= size &&
        !parse_decimal(text, 0u, semicolon, &number)) {
        return false;
    }

    event->key = final == '~' ?
        legacy_tilde_key(number) : legacy_letter_key(final);
    if (event->key == KITTYKB_KEY_NONE) {
        return false;
    }
    if (final != '~' && size != 0u && number != 1u) {
        return false;
    }
    if (semicolon < size &&
        !parse_modifier_action(
            text,
            semicolon + 1u,
            size,
            &event->modifiers,
            &event->action,
            &event->event_type_explicit)) {
        return false;
    }
    if (final == 'Z') {
        event->modifiers |= KITTYKB_MOD_SHIFT;
    }
    return true;
}

static bool
parse_keyboard_flags_response(kittykb_input *input)
{
    uint32_t flags;

    if (input->sequence_length < 2u || input->sequence[0] != '?' ||
        !parse_decimal(
            input->sequence,
            1u,
            input->sequence_length,
            &flags)) {
        return false;
    }
    kittykb_input_set_terminal_flags(input, flags);
    return true;
}

static void
finish_csi(kittykb_input *input, char final)
{
    kittykb_event event;
    bool valid = false;
    bool recognized = false;

    if (final == 'u' && input->sequence_length != 0u &&
        input->sequence[0] == '?') {
        recognized = true;
        valid = parse_keyboard_flags_response(input);
    } else if (final == 'u') {
        recognized = true;
        valid = parse_kitty_key(
            input->sequence,
            input->sequence_length,
            &event);
        if (valid) {
            emit_event(input, &event);
        }
    } else if (final == 'c' && input->sequence_length != 0u &&
               (input->sequence[0] == '?' ||
                input->sequence[0] == '>')) {
        recognized = true;
        valid = true;
        input->saw_primary_device_attributes = true;
        if (input->support == (uint8_t)KITTYKB_SUPPORT_UNKNOWN) {
            input->support = (uint8_t)KITTYKB_SUPPORT_UNAVAILABLE;
            input->terminal_flags = 0u;
            kittykb_input_release_all(input);
        }
    } else if (final == '~' || final == 'A' || final == 'B' ||
               final == 'C' || final == 'D' || final == 'E' ||
               final == 'F' || final == 'H' || final == 'P' ||
               final == 'Q' || final == 'S' || final == 'Z') {
        recognized = true;
        valid = parse_legacy_key(
            input->sequence,
            input->sequence_length,
            final,
            &event);
        if (valid) {
            emit_event(input, &event);
        }
    }

    if (!recognized) {
        ++input->stats.ignored_sequences;
    } else if (!valid) {
        ++input->stats.malformed_sequences;
    }
    input->parser_state = (uint8_t)PARSER_GROUND;
    input->sequence_length = 0u;
}

static void
emit_codepoint(
    kittykb_input *input,
    uint32_t value,
    uint32_t modifiers)
{
    kittykb_event event;

    (void)memset(&event, 0, sizeof(event));
    event.key = value;
    event.modifiers = modifiers;
    event.action = KITTYKB_ACTION_PRESS;
    event.source = (uint8_t)KITTYKB_SOURCE_LEGACY;
    if (is_allowed_text_codepoint(value)) {
        event.text[0] = value;
        event.text_length = 1u;
    }
    emit_event(input, &event);
}

static void
feed_ground_byte(
    kittykb_input *input,
    unsigned char byte,
    uint32_t modifiers)
{
    if (byte == (unsigned char)'\r' || byte == (unsigned char)'\n') {
        emit_simple(
            input,
            KITTYKB_KEY_ENTER,
            modifiers,
            KITTYKB_ACTION_PRESS,
            KITTYKB_SOURCE_LEGACY,
            false);
    } else if (byte == (unsigned char)'\t') {
        emit_simple(
            input,
            KITTYKB_KEY_TAB,
            modifiers,
            KITTYKB_ACTION_PRESS,
            KITTYKB_SOURCE_LEGACY,
            false);
    } else if (byte == 127u || byte == 8u) {
        emit_simple(
            input,
            KITTYKB_KEY_BACKSPACE,
            modifiers,
            KITTYKB_ACTION_PRESS,
            KITTYKB_SOURCE_LEGACY,
            false);
    } else if (byte < 0x80u) {
        emit_codepoint(input, (uint32_t)byte, modifiers);
    } else if (byte >= 0xc2u && byte <= 0xdfu) {
        input->utf8_value = (uint32_t)(byte & 0x1fu);
        input->utf8_minimum = 0x80u;
        input->utf8_remaining = 1u;
        input->utf8_modifiers = modifiers;
    } else if (byte >= 0xe0u && byte <= 0xefu) {
        input->utf8_value = (uint32_t)(byte & 0x0fu);
        input->utf8_minimum = 0x800u;
        input->utf8_remaining = 2u;
        input->utf8_modifiers = modifiers;
    } else if (byte >= 0xf0u && byte <= 0xf4u) {
        input->utf8_value = (uint32_t)(byte & 0x07u);
        input->utf8_minimum = 0x10000u;
        input->utf8_remaining = 3u;
        input->utf8_modifiers = modifiers;
    } else {
        ++input->stats.malformed_sequences;
    }
}

static void feed_byte(kittykb_input *input, unsigned char byte);

static void
feed_utf8_continuation(kittykb_input *input, unsigned char byte)
{
    if ((byte & 0xc0u) != 0x80u) {
        input->utf8_remaining = 0u;
        ++input->stats.malformed_sequences;
        feed_byte(input, byte);
        return;
    }
    input->utf8_value =
        (input->utf8_value << 6u) | (uint32_t)(byte & 0x3fu);
    --input->utf8_remaining;
    if (input->utf8_remaining == 0u) {
        const uint32_t value = input->utf8_value;

        if (value < input->utf8_minimum || !is_unicode_scalar(value)) {
            ++input->stats.malformed_sequences;
        } else {
            emit_codepoint(input, value, input->utf8_modifiers);
        }
    }
}

static void
start_control_string(kittykb_input *input, enum string_kind kind)
{
    input->parser_state = (uint8_t)PARSER_STRING;
    input->string_kind = (uint8_t)kind;
    input->sequence_length = 0u;
}

static void
feed_escape_byte(kittykb_input *input, unsigned char byte)
{
    switch (byte) {
    case '[':
        input->parser_state = (uint8_t)PARSER_CSI;
        input->sequence_length = 0u;
        break;
    case 'O':
        input->parser_state = (uint8_t)PARSER_SS3;
        break;
    case ']':
        start_control_string(input, STRING_OSC);
        break;
    case 'P':
    case '_':
    case '^':
    case 'X':
        start_control_string(input, STRING_OTHER);
        break;
    case '\\':
        input->parser_state = (uint8_t)PARSER_GROUND;
        ++input->stats.ignored_sequences;
        break;
    case 0x1b:
        emit_simple(
            input,
            KITTYKB_KEY_ESCAPE,
            KITTYKB_MOD_ALT,
            KITTYKB_ACTION_PRESS,
            KITTYKB_SOURCE_LEGACY,
            false);
        input->parser_state = (uint8_t)PARSER_GROUND;
        break;
    default:
        input->parser_state = (uint8_t)PARSER_GROUND;
        feed_ground_byte(input, byte, KITTYKB_MOD_ALT);
        break;
    }
}

static void
feed_ss3_byte(kittykb_input *input, unsigned char byte)
{
    uint32_t key = KITTYKB_KEY_NONE;

    switch (byte) {
    case 'A':
        key = KITTYKB_KEY_UP;
        break;
    case 'B':
        key = KITTYKB_KEY_DOWN;
        break;
    case 'C':
        key = KITTYKB_KEY_RIGHT;
        break;
    case 'D':
        key = KITTYKB_KEY_LEFT;
        break;
    case 'E':
        key = KITTYKB_KEY_KP_BEGIN;
        break;
    case 'F':
        key = KITTYKB_KEY_END;
        break;
    case 'H':
        key = KITTYKB_KEY_HOME;
        break;
    case 'P':
        key = KITTYKB_KEY_F1;
        break;
    case 'Q':
        key = KITTYKB_KEY_F2;
        break;
    case 'R':
        key = KITTYKB_KEY_F3;
        break;
    case 'S':
        key = KITTYKB_KEY_F4;
        break;
    default:
        break;
    }
    if (key == KITTYKB_KEY_NONE) {
        ++input->stats.ignored_sequences;
    } else {
        emit_simple(
            input,
            key,
            0u,
            KITTYKB_ACTION_PRESS,
            KITTYKB_SOURCE_LEGACY,
            false);
    }
    input->parser_state = (uint8_t)PARSER_GROUND;
}

static void
feed_byte(kittykb_input *input, unsigned char byte)
{
    if (input->utf8_remaining != 0u) {
        feed_utf8_continuation(input, byte);
        return;
    }

    switch ((enum parser_state)input->parser_state) {
    case PARSER_GROUND:
        if (byte == 0x1bu) {
            input->parser_state = (uint8_t)PARSER_ESCAPE;
        } else if (byte == 0x9bu) {
            input->parser_state = (uint8_t)PARSER_CSI;
            input->sequence_length = 0u;
        } else if (byte == 0x9du) {
            start_control_string(input, STRING_OSC);
        } else if (byte == 0x90u || byte == 0x98u ||
                   byte == 0x9eu || byte == 0x9fu) {
            start_control_string(input, STRING_OTHER);
        } else {
            feed_ground_byte(input, byte, 0u);
        }
        break;

    case PARSER_ESCAPE:
        feed_escape_byte(input, byte);
        break;

    case PARSER_CSI:
        if (byte >= 0x40u && byte <= 0x7eu) {
            finish_csi(input, (char)byte);
        } else if (byte == 0x1bu) {
            ++input->stats.malformed_sequences;
            input->parser_state = (uint8_t)PARSER_ESCAPE;
            input->sequence_length = 0u;
        } else if (byte >= 0x20u && byte <= 0x3fu) {
            if (input->sequence_length >= KITTYKB_SEQUENCE_CAPACITY) {
                ++input->stats.malformed_sequences;
                input->parser_state = (uint8_t)PARSER_CSI_DISCARD;
                input->sequence_length = 0u;
            } else {
                input->sequence[input->sequence_length] = (char)byte;
                ++input->sequence_length;
            }
        } else {
            ++input->stats.malformed_sequences;
            input->parser_state = (uint8_t)PARSER_GROUND;
            input->sequence_length = 0u;
        }
        break;

    case PARSER_CSI_DISCARD:
        if (byte >= 0x40u && byte <= 0x7eu) {
            input->parser_state = (uint8_t)PARSER_GROUND;
        } else if (byte == 0x1bu) {
            input->parser_state = (uint8_t)PARSER_ESCAPE;
        }
        break;

    case PARSER_SS3:
        feed_ss3_byte(input, byte);
        break;

    case PARSER_STRING:
        if (byte == 0x1bu) {
            input->parser_state = (uint8_t)PARSER_STRING_ESCAPE;
        } else if (byte == 0x9cu ||
                   (input->string_kind == (uint8_t)STRING_OSC &&
                    byte == 0x07u)) {
            input->parser_state = (uint8_t)PARSER_GROUND;
            ++input->stats.ignored_sequences;
        }
        break;

    case PARSER_STRING_ESCAPE:
        if (byte == (unsigned char)'\\' || byte == 0x9cu) {
            input->parser_state = (uint8_t)PARSER_GROUND;
            ++input->stats.ignored_sequences;
        } else if (byte != 0x1bu) {
            input->parser_state = (uint8_t)PARSER_STRING;
        }
        break;
    }
}

void
kittykb_input_init(kittykb_input *input)
{
    if (input == NULL) {
        return;
    }
    (void)memset(input, 0, sizeof(*input));
    input->requested_flags = KITTYKB_FLAGS_DEFAULT;
    input->parser_state = (uint8_t)PARSER_GROUND;
    input->support = (uint8_t)KITTYKB_SUPPORT_UNKNOWN;
}

void
kittykb_input_set_requested_flags(kittykb_input *input, uint32_t flags)
{
    if (input != NULL) {
        input->requested_flags = flags;
    }
}

void
kittykb_input_feed(
    kittykb_input *input,
    const void *bytes,
    size_t byte_count)
{
    const unsigned char *data = bytes;
    size_t index;

    if (input == NULL || (bytes == NULL && byte_count != 0u)) {
        return;
    }
    input->stats.bytes_read += (uint64_t)byte_count;
    for (index = 0u; index < byte_count; ++index) {
        feed_byte(input, data[index]);
    }
}

void
kittykb_input_flush_escape(kittykb_input *input)
{
    if (input == NULL ||
        input->parser_state != (uint8_t)PARSER_ESCAPE) {
        return;
    }
    emit_simple(
        input,
        KITTYKB_KEY_ESCAPE,
        0u,
        KITTYKB_ACTION_PRESS,
        KITTYKB_SOURCE_LEGACY,
        false);
    input->parser_state = (uint8_t)PARSER_GROUND;
}

bool
kittykb_input_has_pending_escape(const kittykb_input *input)
{
    return input != NULL &&
           input->parser_state == (uint8_t)PARSER_ESCAPE;
}

bool
kittykb_input_next(kittykb_input *input, kittykb_event *event)
{
    if (input == NULL || event == NULL || input->queue_count == 0u) {
        return false;
    }
    *event = input->queue[input->queue_head];
    input->queue_head =
        (input->queue_head + 1u) % KITTYKB_EVENT_QUEUE_CAPACITY;
    --input->queue_count;
    return true;
}

void
kittykb_input_clear_events(kittykb_input *input)
{
    if (input != NULL) {
        input->queue_head = 0u;
        input->queue_count = 0u;
    }
}

bool
kittykb_input_has_release_events(const kittykb_input *input)
{
    const uint32_t needed =
        KITTYKB_FLAG_REPORT_EVENTS | KITTYKB_FLAG_REPORT_ALL_KEYS;

    return input != NULL && input->support == (uint8_t)KITTYKB_SUPPORT_AVAILABLE &&
           (input->terminal_flags & needed) == needed;
}

bool
kittykb_input_key_down(const kittykb_input *input, uint32_t key)
{
    size_t index;

    if (key == KITTYKB_KEY_NONE ||
        !kittykb_input_has_release_events(input)) {
        return false;
    }
    for (index = 0u; index < input->held_count; ++index) {
        const kittykb_held_key *held = &input->held[index];

        if (held->key == key || held->shifted_key == key ||
            held->base_layout_key == key) {
            return true;
        }
    }
    return false;
}

size_t
kittykb_input_held_count(const kittykb_input *input)
{
    if (input == NULL || !kittykb_input_has_release_events(input)) {
        return 0u;
    }
    return input->held_count;
}

void
kittykb_input_release_all(kittykb_input *input)
{
    if (input != NULL) {
        input->held_count = 0u;
    }
}

kittykb_support
kittykb_input_protocol_support(const kittykb_input *input)
{
    if (input == NULL) {
        return KITTYKB_SUPPORT_UNKNOWN;
    }
    return (kittykb_support)input->support;
}

uint32_t
kittykb_input_terminal_flags(const kittykb_input *input)
{
    return input != NULL ? input->terminal_flags : 0u;
}

void
kittykb_input_set_terminal_flags(kittykb_input *input, uint32_t flags)
{
    const uint32_t needed =
        KITTYKB_FLAG_REPORT_EVENTS | KITTYKB_FLAG_REPORT_ALL_KEYS;

    if (input == NULL) {
        return;
    }
    input->terminal_flags = flags;
    input->support = (uint8_t)KITTYKB_SUPPORT_AVAILABLE;
    if ((flags & needed) != needed) {
        kittykb_input_release_all(input);
    }
}

void
kittykb_input_finish_probe(kittykb_input *input)
{
    if (input != NULL &&
        input->support == (uint8_t)KITTYKB_SUPPORT_UNKNOWN) {
        input->support = (uint8_t)KITTYKB_SUPPORT_UNAVAILABLE;
        input->terminal_flags = 0u;
        kittykb_input_release_all(input);
    }
}

const kittykb_stats *
kittykb_input_get_stats(const kittykb_input *input)
{
    return input != NULL ? &input->stats : NULL;
}

bool
kittykb_event_matches_key(const kittykb_event *event, uint32_t key)
{
    return event != NULL && key != KITTYKB_KEY_NONE &&
           (event->key == key || event->shifted_key == key ||
            event->base_layout_key == key);
}

const char *
kittykb_key_name(uint32_t key)
{
    static const char *const names[] = {
        "ESCAPE", "ENTER", "TAB", "BACKSPACE", "INSERT", "DELETE",
        "LEFT", "RIGHT", "UP", "DOWN", "PAGE_UP", "PAGE_DOWN",
        "HOME", "END", "CAPS_LOCK", "SCROLL_LOCK", "NUM_LOCK",
        "PRINT_SCREEN", "PAUSE", "MENU", "F1", "F2", "F3", "F4",
        "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20",
        "F21", "F22", "F23", "F24", "F25", "F26", "F27", "F28",
        "F29", "F30", "F31", "F32", "F33", "F34", "F35",
        "KP_0", "KP_1", "KP_2", "KP_3", "KP_4", "KP_5", "KP_6",
        "KP_7", "KP_8", "KP_9", "KP_DECIMAL", "KP_DIVIDE",
        "KP_MULTIPLY", "KP_SUBTRACT", "KP_ADD", "KP_ENTER",
        "KP_EQUAL", "KP_SEPARATOR", "KP_LEFT", "KP_RIGHT", "KP_UP",
        "KP_DOWN", "KP_PAGE_UP", "KP_PAGE_DOWN", "KP_HOME", "KP_END",
        "KP_INSERT", "KP_DELETE", "KP_BEGIN", "MEDIA_PLAY",
        "MEDIA_PAUSE", "MEDIA_PLAY_PAUSE", "MEDIA_REVERSE", "MEDIA_STOP",
        "MEDIA_FAST_FORWARD", "MEDIA_REWIND", "MEDIA_TRACK_NEXT",
        "MEDIA_TRACK_PREVIOUS", "MEDIA_RECORD", "LOWER_VOLUME",
        "RAISE_VOLUME", "MUTE_VOLUME", "LEFT_SHIFT", "LEFT_CONTROL",
        "LEFT_ALT", "LEFT_SUPER", "LEFT_HYPER", "LEFT_META",
        "RIGHT_SHIFT", "RIGHT_CONTROL", "RIGHT_ALT", "RIGHT_SUPER",
        "RIGHT_HYPER", "RIGHT_META", "ISO_LEVEL3_SHIFT",
        "ISO_LEVEL5_SHIFT"
    };
    const uint32_t first = (uint32_t)KITTYKB_KEY_ESCAPE;
    const uint32_t last = (uint32_t)KITTYKB_KEY_ISO_LEVEL5_SHIFT;

    if (key < first || key > last) {
        return NULL;
    }
    return names[key - first];
}
