#include "kitty_input.h"

#include <limits.h>
#include <string.h>

enum parser_state {
    PARSER_GROUND = 0,
    PARSER_ESCAPE,
    PARSER_CSI,
    PARSER_SS3,
    PARSER_OSC,
    PARSER_OSC_ESCAPE,
    PARSER_STRING,
    PARSER_STRING_ESCAPE,
    PARSER_CSI_DISCARD,
    PARSER_SS3_DISCARD,
    PARSER_PASTE
};

static const unsigned char paste_end[] = {0x1bu, '[', '2', '0', '1', '~'};

#define KITTYIN_KEYBOARD_FEED_BATCH_CAPACITY 8u

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

static void append_paste_bytes(kittyin_input *input,
                               const unsigned char *bytes, size_t size)
{
    size_t offset = 0u;

    while (offset < size) {
        size_t available;
        size_t count;

        if (input->paste_length == KITTYIN_PASTE_CHUNK_CAPACITY)
            emit_paste(input, false);
        available = KITTYIN_PASTE_CHUNK_CAPACITY - input->paste_length;
        count = size - offset < available ? size - offset : available;
        (void)memcpy(input->paste + input->paste_length,
                     bytes + offset, count);
        input->paste_length += count;
        offset += count;
    }
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

static void start_control_string(kittyin_input *input, bool osc)
{
    input->sequence_length = 0u;
    input->parser_state = (uint8_t)(osc ? PARSER_OSC : PARSER_STRING);
}

static void finish_control_string(kittyin_input *input)
{
    ++input->stats.ignored_sequences;
    input->parser_state = (uint8_t)PARSER_GROUND;
}

static bool is_csi_final(unsigned char byte)
{
    return byte >= 0x40u && byte <= 0x7eu;
}

static bool cancels_sequence(unsigned char byte)
{
    return byte == 0x18u || byte == 0x1au;
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
    if (byte_count == 1u &&
        input->parser_state == (uint8_t)PARSER_GROUND &&
        source[0] != 0x1bu) {
        feed_keyboard(input, source, 1u);
        return;
    }

    for (index = 0u; index < byte_count; ++index) {
        const unsigned char byte = source[index];

        switch ((enum parser_state)input->parser_state) {
        case PARSER_GROUND:
            if (byte == 0x1bu) start_escape(input);
            else {
                const size_t remaining = byte_count - index;
                size_t run = 1u;

                /* A byte can produce at most one keyboard event.  Keeping
                 * each batch below the decoder queue size preserves outer
                 * drop accounting while avoiding a call and drain per byte. */
                while (run < remaining &&
                       run < KITTYIN_KEYBOARD_FEED_BATCH_CAPACITY &&
                       source[index + run] != 0x1bu) ++run;
                feed_keyboard(input, source + index, run);
                index += run - 1u;
            }
            break;

        case PARSER_ESCAPE:
            if (byte == (unsigned char)'[') {
                (void)append_sequence(input, byte);
                input->parser_state = (uint8_t)PARSER_CSI;
            } else if (byte == (unsigned char)'O') {
                (void)append_sequence(input, byte);
                input->parser_state = (uint8_t)PARSER_SS3;
            } else if (byte == (unsigned char)']') {
                start_control_string(input, true);
            } else if (byte == (unsigned char)'P' ||
                       byte == (unsigned char)'^' ||
                       byte == (unsigned char)'_' ||
                       byte == (unsigned char)'X') {
                start_control_string(input, false);
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
            if (byte == 0x1bu) {
                ++input->stats.malformed_sequences;
                start_escape(input);
            } else if (cancels_sequence(byte)) {
                ++input->stats.malformed_sequences;
                input->sequence_length = 0u;
                input->parser_state = (uint8_t)PARSER_GROUND;
            } else if (!append_sequence(input, byte)) {
                input->parser_state = (uint8_t)PARSER_CSI_DISCARD;
            } else if (is_csi_final(byte)) {
                input->parser_state = (uint8_t)PARSER_GROUND;
                handle_csi(input);
                if (input->parser_state != (uint8_t)PARSER_PASTE)
                    input->sequence_length = 0u;
            }
            break;

        case PARSER_SS3:
            if (byte == 0x1bu) {
                ++input->stats.malformed_sequences;
                start_escape(input);
            } else if (cancels_sequence(byte)) {
                ++input->stats.malformed_sequences;
                input->sequence_length = 0u;
                input->parser_state = (uint8_t)PARSER_GROUND;
            } else if (!append_sequence(input, byte)) {
                input->parser_state = (uint8_t)PARSER_SS3_DISCARD;
            } else if (byte >= 0x30u && byte <= 0x7eu) {
                feed_keyboard(input, input->sequence,
                              input->sequence_length);
                input->sequence_length = 0u;
                input->parser_state = (uint8_t)PARSER_GROUND;
            }
            break;

        case PARSER_OSC:
            if (byte == 0x07u || byte == 0x9cu ||
                cancels_sequence(byte))
                finish_control_string(input);
            else if (byte == 0x1bu)
                input->parser_state = (uint8_t)PARSER_OSC_ESCAPE;
            break;

        case PARSER_OSC_ESCAPE:
            if (byte == (unsigned char)'\\' || byte == 0x9cu ||
                byte == 0x07u || cancels_sequence(byte))
                finish_control_string(input);
            else if (byte != 0x1bu)
                input->parser_state = (uint8_t)PARSER_OSC;
            break;

        case PARSER_STRING:
            if (byte == 0x9cu || cancels_sequence(byte))
                finish_control_string(input);
            else if (byte == 0x1bu)
                input->parser_state = (uint8_t)PARSER_STRING_ESCAPE;
            break;

        case PARSER_STRING_ESCAPE:
            if (byte == (unsigned char)'\\' || byte == 0x9cu ||
                cancels_sequence(byte))
                finish_control_string(input);
            else if (byte != 0x1bu)
                input->parser_state = (uint8_t)PARSER_STRING;
            break;

        case PARSER_CSI_DISCARD:
            if (byte == 0x1bu)
                start_escape(input);
            else if (is_csi_final(byte) || cancels_sequence(byte))
                input->parser_state = (uint8_t)PARSER_GROUND;
            break;

        case PARSER_SS3_DISCARD:
            if (byte == 0x1bu)
                start_escape(input);
            else if ((byte >= 0x30u && byte <= 0x7eu) ||
                     cancels_sequence(byte))
                input->parser_state = (uint8_t)PARSER_GROUND;
            break;

        case PARSER_PASTE:
            if (input->paste_match_length == 0u && byte != 0x1bu &&
                index + 1u < byte_count) {
                const size_t remaining = byte_count - index;
                const unsigned char *next_escape =
                    memchr(source + index, 0x1b, remaining);
                const size_t run = next_escape != NULL ?
                    (size_t)(next_escape - (source + index)) : remaining;

                append_paste_bytes(input, source + index, run);
                index += run - 1u;
            } else
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

static uint32_t action_modifier_mask(void)
{
    return KITTYKB_MOD_SHIFT | KITTYKB_MOD_ALT | KITTYKB_MOD_CTRL |
           KITTYKB_MOD_SUPER | KITTYKB_MOD_HYPER | KITTYKB_MOD_META;
}

static bool action_key_matches(const kittyin_action_binding *binding,
                               const kittykb_event *key)
{
    uint32_t modifiers;
    uint32_t control_key;
    bool matches;

    if (binding == NULL || key == NULL) return false;
    matches = kittykb_event_matches_key(key, binding->key);
    if (matches) {
        modifiers = key->modifiers & action_modifier_mask();
        if (modifiers == binding->modifiers) return true;
        return binding->key >= (uint32_t)'a' &&
               binding->key <= (uint32_t)'z' &&
               binding->modifiers == 0u &&
               modifiers == KITTYKB_MOD_SHIFT;
    }
    if (binding->key < (uint32_t)'@' ||
        binding->key > (uint32_t)'z') return false;
    modifiers = key->modifiers & action_modifier_mask();
    if (binding->key >= (uint32_t)'a' &&
        binding->key <= (uint32_t)'z') {
        if (binding->modifiers == 0u && modifiers == 0u &&
            key->key == binding->key - (uint32_t)'a' +
                        (uint32_t)'A') return true;
        control_key = binding->key - (uint32_t)'a' + 1u;
    } else if (binding->key >= (uint32_t)'@' &&
               binding->key <= (uint32_t)'_') {
        control_key = binding->key & UINT32_C(0x1f);
    } else {
        return false;
    }

    /* Legacy terminals encode Ctrl+A through Ctrl+Z as C0 bytes.  Preserve
     * the decoder's raw event representation while allowing semantic
     * bindings to match the equivalent enhanced-protocol key/modifier pair. */
    return key->key == control_key &&
           (modifiers | KITTYKB_MOD_CTRL) == binding->modifiers;
}

static size_t append_action_event(kittyin_action_event *events,
                                  size_t capacity, size_t count,
                                  uint16_t action,
                                  kittyin_action_phase phase,
                                  kittyin_device_kind device,
                                  uint8_t gamepad)
{
    if (events != NULL && count < capacity)
        events[count] = (kittyin_action_event){
            action, phase, device, gamepad
        };
    return count < capacity ? count + 1u : count;
}

void kittyin_action_map_init(kittyin_action_map *map)
{
    if (map == NULL) return;
    (void)memset(map, 0, sizeof *map);
    map->repeat_delay_ms = 320u;
    map->repeat_interval_ms = 85u;
    map->axis_dead_zone = INT16_C(12000);
    map->active_gamepad = KITTYIN_GAMEPAD_ANY;
    map->active_device = (uint8_t)KITTYIN_DEVICE_KEYBOARD;
}

static bool binding_equals(const kittyin_action_binding *binding,
                           uint16_t action, uint8_t kind,
                           uint32_t key, uint32_t modifiers,
                           uint8_t gamepad, uint8_t control,
                           int direction)
{
    return binding != NULL && binding->action == action &&
           binding->kind == kind && binding->key == key &&
           binding->modifiers == modifiers &&
           binding->gamepad == gamepad &&
           binding->control == control &&
           binding->direction == direction;
}

static bool append_binding(kittyin_action_map *map, uint16_t action,
                           uint8_t kind, uint32_t key,
                           uint32_t modifiers, uint8_t gamepad,
                           uint8_t control, int direction)
{
    size_t index;

    if (map == NULL || action >= KITTYIN_ACTION_CAPACITY ||
        map->binding_count >= KITTYIN_ACTION_BINDING_CAPACITY)
        return false;
    for (index = 0u; index < map->binding_count; ++index)
        if (binding_equals(&map->bindings[index], action, kind, key,
                           modifiers, gamepad, control, direction))
            return true;
    map->bindings[map->binding_count++] = (kittyin_action_binding){
        key, modifiers, action, kind, gamepad, control,
        (int8_t)direction, 0u
    };
    return true;
}

bool kittyin_action_map_bind_key(kittyin_action_map *map, uint16_t action,
                                 uint32_t key, uint32_t modifiers)
{
    if (key == KITTYKB_KEY_NONE ||
        (modifiers & ~action_modifier_mask()) != 0u) return false;
    return append_binding(map, action, (uint8_t)KITTYIN_BINDING_KEY,
                          key, modifiers, KITTYIN_GAMEPAD_ANY, 0u, 0);
}

static bool valid_gamepad_binding(uint8_t gamepad)
{
    return gamepad == KITTYIN_GAMEPAD_ANY ||
           gamepad < KITTYIN_GAMEPAD_CAPACITY;
}

bool kittyin_action_map_bind_gamepad_button(kittyin_action_map *map,
                                            uint16_t action,
                                            uint8_t gamepad,
                                            uint8_t button)
{
    if (!valid_gamepad_binding(gamepad)) return false;
    return append_binding(
        map, action, (uint8_t)KITTYIN_BINDING_GAMEPAD_BUTTON,
        0u, 0u, gamepad, button, 0);
}

bool kittyin_action_map_bind_gamepad_axis(kittyin_action_map *map,
                                          uint16_t action,
                                          uint8_t gamepad, uint8_t axis,
                                          int direction)
{
    if (!valid_gamepad_binding(gamepad) ||
        (direction != -1 && direction != 1)) return false;
    return append_binding(
        map, action, (uint8_t)KITTYIN_BINDING_GAMEPAD_AXIS,
        0u, 0u, gamepad, axis, direction);
}

static unsigned int action_popcount(uint8_t value)
{
    unsigned int count = 0u;

    while (value != 0u) {
        count += (unsigned int)(value & UINT8_C(1));
        value = (uint8_t)(value >> 1u);
    }
    return count;
}

static bool action_has_gamepad_source(const kittyin_action_map *map,
                                      uint16_t action, uint8_t gamepad)
{
    uint8_t bit = 0u;
    size_t index;

    if (gamepad < KITTYIN_GAMEPAD_CAPACITY)
        bit = (uint8_t)(UINT8_C(1) << (unsigned int)gamepad);
    if (bit == 0u) return false;
    for (index = 0u; index < map->binding_count; ++index) {
        const kittyin_action_binding *binding = &map->bindings[index];

        if (binding->action == action &&
            binding->kind != (uint8_t)KITTYIN_BINDING_KEY &&
            (binding->down_gamepads & bit) != 0u) return true;
    }
    return false;
}

static void repair_action_gamepad(kittyin_action_map *map, uint16_t action)
{
    kittyin_action_state *state = &map->actions[action];
    size_t index;

    if (state->sources == 0u ||
        action_has_gamepad_source(map, action, state->gamepad)) return;
    for (index = 0u; index < map->binding_count; ++index) {
        const kittyin_action_binding *binding = &map->bindings[index];
        uint8_t gamepad;

        if (binding->action != action ||
            binding->kind == (uint8_t)KITTYIN_BINDING_KEY) continue;
        for (gamepad = 0u; gamepad < KITTYIN_GAMEPAD_CAPACITY; ++gamepad)
            if ((binding->down_gamepads &
                 (uint8_t)(UINT8_C(1) << gamepad)) != 0u) {
                state->gamepad = gamepad;
                return;
            }
    }
}

size_t kittyin_action_map_unbind(kittyin_action_map *map, uint16_t action,
                                 kittyin_device_kind device)
{
    size_t read_index;
    size_t write_index = 0u;
    size_t removed = 0u;

    if (map == NULL || action >= KITTYIN_ACTION_CAPACITY ||
        (device != KITTYIN_DEVICE_KEYBOARD &&
         device != KITTYIN_DEVICE_GAMEPAD)) return 0u;
    for (read_index = 0u; read_index < map->binding_count; ++read_index) {
        kittyin_action_binding binding = map->bindings[read_index];
        const bool is_keyboard =
            binding.kind == (uint8_t)KITTYIN_BINDING_KEY;
        const bool selected =
            binding.action == action &&
            ((device == KITTYIN_DEVICE_KEYBOARD && is_keyboard) ||
             (device == KITTYIN_DEVICE_GAMEPAD && !is_keyboard));

        if (selected) {
            kittyin_action_state *state = &map->actions[action];
            const unsigned int down =
                action_popcount(binding.down_gamepads);

            state->sources = state->sources > down ?
                (uint16_t)(state->sources - down) : 0u;
            if (state->sources == 0u)
                *state = (kittyin_action_state){0};
            ++removed;
            continue;
        }
        map->bindings[write_index++] = binding;
    }
    map->binding_count = write_index;
    if (map->actions[action].sources != 0u)
        repair_action_gamepad(map, action);
    return removed;
}

bool kittyin_action_map_has_binding(const kittyin_action_map *map,
                                    uint16_t action,
                                    kittyin_device_kind device)
{
    size_t index;

    if (map == NULL || action >= KITTYIN_ACTION_CAPACITY) return false;
    for (index = 0u; index < map->binding_count; ++index) {
        const kittyin_action_binding *binding = &map->bindings[index];
        const bool is_keyboard =
            binding->kind == (uint8_t)KITTYIN_BINDING_KEY;

        if (binding->action == action &&
            ((device == KITTYIN_DEVICE_KEYBOARD && is_keyboard) ||
             (device == KITTYIN_DEVICE_GAMEPAD && !is_keyboard)))
            return true;
    }
    return false;
}

bool kittyin_action_map_set_repeat(kittyin_action_map *map,
                                   uint32_t delay_ms,
                                   uint32_t interval_ms)
{
    if (map == NULL || delay_ms < 50u || delay_ms > 5000u ||
        interval_ms < 16u || interval_ms > 1000u) return false;
    map->repeat_delay_ms = delay_ms;
    map->repeat_interval_ms = interval_ms;
    return true;
}

bool kittyin_action_map_set_dead_zone(kittyin_action_map *map,
                                      int dead_zone)
{
    if (map == NULL || dead_zone < 1 || dead_zone > INT16_MAX)
        return false;
    map->axis_dead_zone = (int16_t)dead_zone;
    return true;
}

size_t kittyin_action_map_key(kittyin_action_map *map,
                              const kittykb_event *key,
                              kittyin_action_event *events,
                              size_t capacity)
{
    bool emitted[KITTYIN_ACTION_CAPACITY] = {false};
    bool matched = false;
    kittyin_action_phase phase;
    size_t count = 0u;
    size_t index;

    if (map == NULL || key == NULL ||
        (events == NULL && capacity != 0u)) return 0u;
    if (key->action == KITTYKB_ACTION_PRESS)
        phase = KITTYIN_ACTION_PRESS;
    else if (key->action == KITTYKB_ACTION_REPEAT)
        phase = KITTYIN_ACTION_REPEAT;
    else if (key->action == KITTYKB_ACTION_RELEASE)
        phase = KITTYIN_ACTION_RELEASE;
    else
        return 0u;
    for (index = 0u; index < map->binding_count; ++index) {
        const kittyin_action_binding *binding = &map->bindings[index];

        if (binding->kind != (uint8_t)KITTYIN_BINDING_KEY ||
            emitted[binding->action] ||
            !action_key_matches(binding, key)) continue;
        matched = true;
        emitted[binding->action] = true;
        count = append_action_event(
            events, capacity, count, binding->action, phase,
            KITTYIN_DEVICE_KEYBOARD, KITTYIN_GAMEPAD_ANY);
    }
    if (matched && phase != KITTYIN_ACTION_RELEASE) {
        map->active_device = (uint8_t)KITTYIN_DEVICE_KEYBOARD;
        map->active_gamepad = KITTYIN_GAMEPAD_ANY;
    }
    return count;
}

static bool binding_accepts_gamepad(
    const kittyin_action_binding *binding, uint8_t gamepad)
{
    return binding->gamepad == KITTYIN_GAMEPAD_ANY ||
           binding->gamepad == gamepad;
}

static size_t set_gamepad_binding(
    kittyin_action_map *map, kittyin_action_binding *binding,
    uint8_t gamepad, bool down, kittyin_action_event *events,
    size_t capacity, size_t count)
{
    const uint8_t bit =
        (uint8_t)(UINT8_C(1) << (unsigned int)gamepad);
    const bool was_down = (binding->down_gamepads & bit) != 0u;
    kittyin_action_state *state;

    if (was_down == down) return count;
    state = &map->actions[binding->action];
    if (down) {
        binding->down_gamepads |= bit;
        if (state->sources < UINT16_MAX) ++state->sources;
        state->gamepad = gamepad;
        if (state->sources == 1u) {
            state->held_ms = 0u;
            state->next_repeat_ms = map->repeat_delay_ms;
            count = append_action_event(
                events, capacity, count, binding->action,
                KITTYIN_ACTION_PRESS, KITTYIN_DEVICE_GAMEPAD, gamepad);
        }
        map->active_device = (uint8_t)KITTYIN_DEVICE_GAMEPAD;
        map->active_gamepad = gamepad;
    } else {
        binding->down_gamepads &= (uint8_t)~bit;
        if (state->sources > 0u) --state->sources;
        if (state->sources == 0u) {
            count = append_action_event(
                events, capacity, count, binding->action,
                KITTYIN_ACTION_RELEASE, KITTYIN_DEVICE_GAMEPAD, gamepad);
            *state = (kittyin_action_state){0};
        } else
            repair_action_gamepad(map, binding->action);
    }
    return count;
}

static uint8_t first_connected_gamepad(uint8_t mask)
{
    uint8_t gamepad;

    for (gamepad = 0u; gamepad < KITTYIN_GAMEPAD_CAPACITY; ++gamepad)
        if ((mask & (uint8_t)(UINT8_C(1) << gamepad)) != 0u)
            return gamepad;
    return KITTYIN_GAMEPAD_ANY;
}

size_t kittyin_action_map_gamepad(kittyin_action_map *map,
                                  const kittyin_gamepad_event *gamepad,
                                  kittyin_action_event *events,
                                  size_t capacity)
{
    const uint8_t bit = (uint8_t)(
        gamepad != NULL && gamepad->gamepad < KITTYIN_GAMEPAD_CAPACITY ?
        (UINT32_C(1) << gamepad->gamepad) : UINT32_C(0));
    size_t count = 0u;
    size_t index;

    if (map == NULL || gamepad == NULL || bit == 0u ||
        (events == NULL && capacity != 0u)) return 0u;
    if (gamepad->kind == KITTYIN_GAMEPAD_CONNECTED) {
        map->connected_gamepads |= bit;
        return 0u;
    }
    if (gamepad->kind == KITTYIN_GAMEPAD_DISCONNECTED) {
        for (index = 0u; index < map->binding_count; ++index) {
            kittyin_action_binding *binding = &map->bindings[index];

            if (binding->kind == (uint8_t)KITTYIN_BINDING_KEY ||
                (binding->down_gamepads & bit) == 0u) continue;
            count = set_gamepad_binding(
                map, binding, gamepad->gamepad, false,
                events, capacity, count);
        }
        map->connected_gamepads &= (uint8_t)~bit;
        if (map->active_device == (uint8_t)KITTYIN_DEVICE_GAMEPAD &&
            map->active_gamepad == gamepad->gamepad) {
            map->active_gamepad =
                first_connected_gamepad(map->connected_gamepads);
            if (map->active_gamepad == KITTYIN_GAMEPAD_ANY)
                map->active_device = (uint8_t)KITTYIN_DEVICE_KEYBOARD;
        }
        return count;
    }
    if (gamepad->initial ||
        (gamepad->kind != KITTYIN_GAMEPAD_BUTTON &&
         gamepad->kind != KITTYIN_GAMEPAD_AXIS))
        return 0u;
    map->connected_gamepads |= bit;
    for (index = 0u; index < map->binding_count; ++index) {
        kittyin_action_binding *binding = &map->bindings[index];
        bool down;

        if (!binding_accepts_gamepad(binding, gamepad->gamepad) ||
            binding->control != gamepad->control) continue;
        if (gamepad->kind == KITTYIN_GAMEPAD_BUTTON) {
            if (binding->kind !=
                (uint8_t)KITTYIN_BINDING_GAMEPAD_BUTTON) continue;
            down = gamepad->value != 0;
        } else {
            const int value = (int)gamepad->value;
            const int dead_zone = (int)map->axis_dead_zone;

            if (binding->kind !=
                (uint8_t)KITTYIN_BINDING_GAMEPAD_AXIS) continue;
            down = binding->direction < 0 ?
                value <= -dead_zone : value >= dead_zone;
        }
        count = set_gamepad_binding(
            map, binding, gamepad->gamepad, down,
            events, capacity, count);
    }
    return count;
}

size_t kittyin_action_map_advance(kittyin_action_map *map,
                                  uint32_t elapsed_ms,
                                  kittyin_action_event *events,
                                  size_t capacity)
{
    size_t count = 0u;
    size_t action;

    if (map == NULL || (events == NULL && capacity != 0u)) return 0u;
    for (action = 0u; action < KITTYIN_ACTION_CAPACITY; ++action) {
        kittyin_action_state *state = &map->actions[action];
        uint64_t held;
        uint64_t next;

        if (state->sources == 0u || state->next_repeat_ms == 0u) continue;
        held = (uint64_t)state->held_ms + elapsed_ms;
        state->held_ms =
            held > UINT32_MAX ? UINT32_MAX : (uint32_t)held;
        if (state->held_ms < state->next_repeat_ms) continue;
        count = append_action_event(
            events, capacity, count, (uint16_t)action,
            KITTYIN_ACTION_REPEAT, KITTYIN_DEVICE_GAMEPAD,
            state->gamepad);
        next = (uint64_t)state->next_repeat_ms +
               ((((uint64_t)state->held_ms - state->next_repeat_ms) /
                 map->repeat_interval_ms) + 1u) *
               map->repeat_interval_ms;
        state->next_repeat_ms =
            next > UINT32_MAX ? 0u : (uint32_t)next;
        map->active_device = (uint8_t)KITTYIN_DEVICE_GAMEPAD;
        map->active_gamepad = state->gamepad;
    }
    return count;
}

size_t kittyin_action_map_release_all(kittyin_action_map *map,
                                      kittyin_action_event *events,
                                      size_t capacity)
{
    size_t count = 0u;
    size_t index;

    if (map == NULL || (events == NULL && capacity != 0u)) return 0u;
    for (index = 0u; index < map->binding_count; ++index) {
        kittyin_action_binding *binding = &map->bindings[index];
        uint8_t gamepad;

        if (binding->kind == (uint8_t)KITTYIN_BINDING_KEY) continue;
        for (gamepad = 0u;
             gamepad < KITTYIN_GAMEPAD_CAPACITY; ++gamepad)
            if ((binding->down_gamepads &
                 (uint8_t)(UINT8_C(1) << gamepad)) != 0u)
                count = set_gamepad_binding(
                    map, binding, gamepad, false,
                    events, capacity, count);
    }
    return count;
}

kittyin_device_kind
kittyin_action_map_active_device(const kittyin_action_map *map)
{
    return map != NULL &&
           map->active_device == (uint8_t)KITTYIN_DEVICE_GAMEPAD ?
           KITTYIN_DEVICE_GAMEPAD : KITTYIN_DEVICE_KEYBOARD;
}

uint8_t kittyin_action_map_active_gamepad(const kittyin_action_map *map)
{
    return map != NULL ? map->active_gamepad : KITTYIN_GAMEPAD_ANY;
}
