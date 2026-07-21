#include "kitty_input.h"

#include <limits.h>
#include <string.h>

enum parser_state {
    PARSER_GROUND = 0,
    PARSER_ESCAPE,
    PARSER_CSI,
    PARSER_SS3,
    PARSER_STRING,
    PARSER_STRING_ESCAPE,
    PARSER_CSI_DISCARD,
    PARSER_SS3_DISCARD,
    PARSER_PASTE
};

static const unsigned char paste_end[] = {0x1bu, '[', '2', '0', '1', '~'};

static void emit_event(kittyin_input *input, const kittyin_event *event)
{
    size_t tail;

    ++input->stats.events_decoded;
    if (input->queue_count >= KITTYIN_EVENT_QUEUE_CAPACITY) {
        ++input->stats.dropped_events;
        return;
    }
    tail = (input->queue_head + input->queue_count) %
           KITTYIN_EVENT_QUEUE_CAPACITY;
    input->queue[tail] = *event;
    ++input->queue_count;
}

static void drain_keyboard(kittyin_input *input)
{
    kittykb_event key;

    if (input == NULL || input->keyboard == NULL) return;
    while (kittykb_input_next(input->keyboard, &key)) {
        kittyin_event event;

        (void)memset(&event, 0, sizeof event);
        event.kind = KITTYIN_EVENT_KEY;
        event.data.key = key;
        emit_event(input, &event);
    }
}

static void feed_keyboard(kittyin_input *input, const void *bytes, size_t size)
{
    kittykb_input_feed(input->keyboard, bytes, size);
    drain_keyboard(input);
}

void kittyin_input_init(kittyin_input *input)
{
    if (input == NULL) return;
    (void)memset(input, 0, sizeof *input);
    kittykb_input_init(&input->keyboard_storage);
    input->keyboard = &input->keyboard_storage;
    input->pixel_coordinates = true;
}

void kittyin_input_bind_keyboard(kittyin_input *input, kittykb_input *keyboard)
{
    if (input == NULL) return;
    input->keyboard = keyboard != NULL ? keyboard : &input->keyboard_storage;
    drain_keyboard(input);
}

kittykb_input *kittyin_input_keyboard(kittyin_input *input)
{
    return input != NULL ? input->keyboard : NULL;
}

const kittykb_input *kittyin_input_keyboard_const(const kittyin_input *input)
{
    return input != NULL ? input->keyboard : NULL;
}

void kittyin_input_set_pixel_coordinates(kittyin_input *input, bool enabled)
{
    if (input != NULL) input->pixel_coordinates = enabled;
}

static bool parse_decimal(const char *text, size_t begin, size_t end,
                          uint32_t *value)
{
    uint32_t result = 0u;
    size_t index;

    if (text == NULL || value == NULL || begin >= end) return false;
    for (index = begin; index < end; ++index) {
        const unsigned char character = (unsigned char)text[index];
        const uint32_t digit = (uint32_t)(character - (unsigned char)'0');

        if (character < (unsigned char)'0' ||
            character > (unsigned char)'9' ||
            result > (UINT32_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    *value = result;
    return true;
}

static bool parse_mouse(const char *sequence, size_t length,
                        kittyin_mouse_event *mouse)
{
    size_t separators[2];
    size_t separator_count = 0u;
    size_t index;
    uint32_t values[3];
    uint32_t code;
    const unsigned char final = (unsigned char)sequence[length - 1u];

    if (length < 7u || sequence[0] != '\x1b' || sequence[1] != '[' ||
        sequence[2] != '<' || (final != (unsigned char)'M' &&
                               final != (unsigned char)'m')) return false;
    for (index = 3u; index + 1u < length; ++index) {
        if (sequence[index] == ';') {
            if (separator_count >= 2u) return false;
            separators[separator_count++] = index;
        }
    }
    if (separator_count != 2u ||
        !parse_decimal(sequence, 3u, separators[0], &values[0]) ||
        !parse_decimal(sequence, separators[0] + 1u, separators[1],
                       &values[1]) ||
        !parse_decimal(sequence, separators[1] + 1u, length - 1u,
                       &values[2]) ||
        values[1] == 0u || values[2] == 0u ||
        values[1] > (uint32_t)INT32_MAX ||
        values[2] > (uint32_t)INT32_MAX) return false;

    (void)memset(mouse, 0, sizeof *mouse);
    mouse->x = (int32_t)(values[1] - 1u);
    mouse->y = (int32_t)(values[2] - 1u);
    mouse->raw_button = values[0];
    if ((values[0] & 4u) != 0u) mouse->modifiers |= KITTYKB_MOD_SHIFT;
    if ((values[0] & 8u) != 0u) mouse->modifiers |= KITTYKB_MOD_ALT;
    if ((values[0] & 16u) != 0u) mouse->modifiers |= KITTYKB_MOD_CTRL;
    code = values[0] & 3u;

    if ((values[0] & 64u) != 0u) {
        mouse->action = (uint8_t)KITTYIN_MOUSE_WHEEL;
        mouse->button = (uint8_t)(4u + code);
        if (code == 0u) mouse->wheel_y = -1;
        else if (code == 1u) mouse->wheel_y = 1;
        else if (code == 2u) mouse->wheel_x = -1;
        else mouse->wheel_x = 1;
    } else {
        if ((values[0] & 128u) != 0u)
            mouse->button = (uint8_t)(8u + code);
        else if (code != 3u)
            mouse->button = (uint8_t)(code + 1u);
        if ((values[0] & 32u) != 0u)
            mouse->action = (uint8_t)KITTYIN_MOUSE_MOVE;
        else if (final == (unsigned char)'m' || code == 3u)
            mouse->action = (uint8_t)KITTYIN_MOUSE_RELEASE;
        else
            mouse->action = (uint8_t)KITTYIN_MOUSE_PRESS;
    }
    return true;
}

static void emit_focus(kittyin_input *input, bool focused)
{
    kittyin_event event;

    (void)memset(&event, 0, sizeof event);
    event.kind = KITTYIN_EVENT_FOCUS;
    event.data.focus.focused = focused;
    emit_event(input, &event);
}

static void emit_paste(kittyin_input *input, bool final)
{
    kittyin_event event;

    if (input->paste_length == 0u && !final) return;
    (void)memset(&event, 0, sizeof event);
    event.kind = KITTYIN_EVENT_PASTE;
    event.data.paste.length = input->paste_length;
    event.data.paste.final = final;
    if (input->paste_length != 0u)
        (void)memcpy(event.data.paste.bytes, input->paste,
                     input->paste_length);
    emit_event(input, &event);
    ++input->stats.paste_chunks;
    input->paste_length = 0u;
}

static void append_paste_byte(kittyin_input *input, unsigned char byte)
{
    if (input->paste_length >= KITTYIN_PASTE_CHUNK_CAPACITY)
        emit_paste(input, false);
    input->paste[input->paste_length++] = byte;
}

static void handle_paste_byte(kittyin_input *input, unsigned char byte)
{
    for (;;) {
        if (byte == paste_end[input->paste_match_length]) {
            ++input->paste_match_length;
            if (input->paste_match_length == sizeof paste_end) {
                input->paste_match_length = 0u;
                emit_paste(input, true);
                input->parser_state = (uint8_t)PARSER_GROUND;
            }
            return;
        }
        if (input->paste_match_length != 0u) {
            size_t index;

            for (index = 0u; index < input->paste_match_length; ++index)
                append_paste_byte(input, paste_end[index]);
            input->paste_match_length = 0u;
            if (byte == paste_end[0]) continue;
        }
        append_paste_byte(input, byte);
        return;
    }
}

static bool exact_csi(const char *sequence, size_t length, const char *body)
{
    const size_t body_length = strlen(body);

    return length == body_length + 2u && sequence[0] == '\x1b' &&
           sequence[1] == '[' &&
           memcmp(sequence + 2, body, body_length) == 0;
}

static void handle_csi(kittyin_input *input)
{
    kittyin_event event;
    const char final = input->sequence[input->sequence_length - 1u];

    if ((final == 'M' || final == 'm') &&
        input->sequence_length > 2u && input->sequence[2] == '<') {
        (void)memset(&event, 0, sizeof event);
        event.kind = KITTYIN_EVENT_MOUSE;
        if (!parse_mouse(input->sequence, input->sequence_length,
                         &event.data.mouse)) {
            ++input->stats.malformed_sequences;
            return;
        }
        event.data.mouse.pixel_coordinates = input->pixel_coordinates;
        emit_event(input, &event);
        return;
    }
    if (exact_csi(input->sequence, input->sequence_length, "I")) {
        emit_focus(input, true);
        return;
    }
    if (exact_csi(input->sequence, input->sequence_length, "O")) {
        emit_focus(input, false);
        return;
    }
    if (exact_csi(input->sequence, input->sequence_length, "200~")) {
        input->paste_length = 0u;
        input->paste_match_length = 0u;
        input->parser_state = (uint8_t)PARSER_PASTE;
        return;
    }
    if (exact_csi(input->sequence, input->sequence_length, "201~")) {
        ++input->stats.ignored_sequences;
        return;
    }
    feed_keyboard(input, input->sequence, input->sequence_length);
}

static bool append_sequence(kittyin_input *input, unsigned char byte)
{
    if (input->sequence_length >= KITTYIN_SEQUENCE_CAPACITY) {
        ++input->stats.malformed_sequences;
        input->sequence_length = 0u;
        return false;
    }
    input->sequence[input->sequence_length++] = (char)byte;
    return true;
}

static void start_escape(kittyin_input *input)
{
    input->sequence_length = 0u;
    (void)append_sequence(input, 0x1bu);
    input->parser_state = (uint8_t)PARSER_ESCAPE;
}

static bool is_csi_final(unsigned char byte)
{
    return byte >= 0x40u && byte <= 0x7eu;
}

void kittyin_input_feed(kittyin_input *input, const void *bytes,
                        size_t byte_count)
{
    const unsigned char *source = bytes;
    size_t index;

    if (input == NULL || input->keyboard == NULL ||
        (bytes == NULL && byte_count != 0u)) return;
    input->stats.bytes_fed += byte_count;
    drain_keyboard(input);

    for (index = 0u; index < byte_count; ++index) {
        const unsigned char byte = source[index];

        switch ((enum parser_state)input->parser_state) {
        case PARSER_GROUND:
            if (byte == 0x1bu) start_escape(input);
            else feed_keyboard(input, &byte, 1u);
            break;

        case PARSER_ESCAPE:
            if (byte == (unsigned char)'[') {
                (void)append_sequence(input, byte);
                input->parser_state = (uint8_t)PARSER_CSI;
            } else if (byte == (unsigned char)'O') {
                (void)append_sequence(input, byte);
                input->parser_state = (uint8_t)PARSER_SS3;
            } else if (byte == (unsigned char)']' ||
                       byte == (unsigned char)'P' ||
                       byte == (unsigned char)'^' ||
                       byte == (unsigned char)'_') {
                input->sequence_length = 0u;
                input->parser_state = (uint8_t)PARSER_STRING;
            } else if (byte == 0x1bu) {
                const unsigned char escape = 0x1bu;

                feed_keyboard(input, &escape, 1u);
                kittykb_input_flush_escape(input->keyboard);
                drain_keyboard(input);
                start_escape(input);
            } else {
                (void)append_sequence(input, byte);
                feed_keyboard(input, input->sequence,
                              input->sequence_length);
                input->sequence_length = 0u;
                input->parser_state = (uint8_t)PARSER_GROUND;
            }
            break;

        case PARSER_CSI:
            if (!append_sequence(input, byte)) {
                input->parser_state = (uint8_t)PARSER_CSI_DISCARD;
            } else if (is_csi_final(byte)) {
                input->parser_state = (uint8_t)PARSER_GROUND;
                handle_csi(input);
                if (input->parser_state != (uint8_t)PARSER_PASTE)
                    input->sequence_length = 0u;
            }
            break;

        case PARSER_SS3:
            if (!append_sequence(input, byte)) {
                input->parser_state = (uint8_t)PARSER_SS3_DISCARD;
            } else if (byte >= 0x30u && byte <= 0x7eu) {
                feed_keyboard(input, input->sequence,
                              input->sequence_length);
                input->sequence_length = 0u;
                input->parser_state = (uint8_t)PARSER_GROUND;
            }
            break;

        case PARSER_STRING:
            if (byte == 0x07u) {
                ++input->stats.ignored_sequences;
                input->parser_state = (uint8_t)PARSER_GROUND;
            } else if (byte == 0x1bu) {
                input->parser_state = (uint8_t)PARSER_STRING_ESCAPE;
            }
            break;

        case PARSER_STRING_ESCAPE:
            if (byte == (unsigned char)'\\') {
                ++input->stats.ignored_sequences;
                input->parser_state = (uint8_t)PARSER_GROUND;
            } else if (byte != 0x1bu) {
                input->parser_state = (uint8_t)PARSER_STRING;
            }
            break;

        case PARSER_CSI_DISCARD:
            if (is_csi_final(byte))
                input->parser_state = (uint8_t)PARSER_GROUND;
            break;

        case PARSER_SS3_DISCARD:
            if (byte >= 0x30u && byte <= 0x7eu)
                input->parser_state = (uint8_t)PARSER_GROUND;
            break;

        case PARSER_PASTE:
            handle_paste_byte(input, byte);
            break;
        }
    }
    drain_keyboard(input);
}

void kittyin_input_flush_escape(kittyin_input *input)
{
    if (input == NULL || input->keyboard == NULL) return;
    if (input->parser_state == (uint8_t)PARSER_ESCAPE) {
        const unsigned char escape = 0x1bu;

        feed_keyboard(input, &escape, 1u);
        input->sequence_length = 0u;
        input->parser_state = (uint8_t)PARSER_GROUND;
    }
    kittykb_input_flush_escape(input->keyboard);
    drain_keyboard(input);
}

bool kittyin_input_has_pending_escape(const kittyin_input *input)
{
    return input != NULL && input->keyboard != NULL &&
           (input->parser_state == (uint8_t)PARSER_ESCAPE ||
            kittykb_input_has_pending_escape(input->keyboard));
}

bool kittyin_input_next(kittyin_input *input, kittyin_event *event)
{
    if (input == NULL || event == NULL || input->queue_count == 0u)
        return false;
    *event = input->queue[input->queue_head];
    input->queue_head = (input->queue_head + 1u) %
                        KITTYIN_EVENT_QUEUE_CAPACITY;
    --input->queue_count;
    return true;
}

void kittyin_input_clear_events(kittyin_input *input)
{
    if (input == NULL) return;
    input->queue_head = 0u;
    input->queue_count = 0u;
    if (input->keyboard != NULL)
        kittykb_input_clear_events(input->keyboard);
}

void kittyin_input_release_all(kittyin_input *input)
{
    if (input != NULL && input->keyboard != NULL)
        kittykb_input_release_all(input->keyboard);
}

bool kittyin_input_key_down(const kittyin_input *input, uint32_t key)
{
    return input != NULL && input->keyboard != NULL &&
           kittykb_input_key_down(input->keyboard, key);
}

bool kittyin_input_has_release_events(const kittyin_input *input)
{
    return input != NULL && input->keyboard != NULL &&
           kittykb_input_has_release_events(input->keyboard);
}

size_t kittyin_input_held_count(const kittyin_input *input)
{
    return input != NULL && input->keyboard != NULL ?
           kittykb_input_held_count(input->keyboard) : 0u;
}

const kittyin_stats *kittyin_input_get_stats(const kittyin_input *input)
{
    return input != NULL ? &input->stats : NULL;
}
