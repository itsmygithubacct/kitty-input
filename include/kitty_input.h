#ifndef KITTY_INPUT_H
#define KITTY_INPUT_H

/*
 * Ordered, allocation-free terminal input for Kitty applications. Keyboard
 * decoding and held state are provided by kitty-keyboard; this layer adds SGR
 * mouse, focus, and bracketed-paste events without losing wire order.
 */

#include "kitty_keyboard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KITTYIN_VERSION_MAJOR 0
#define KITTYIN_VERSION_MINOR 1
#define KITTYIN_VERSION_PATCH 0

#define KITTYIN_EVENT_QUEUE_CAPACITY 128u
#define KITTYIN_SEQUENCE_CAPACITY 256u
#define KITTYIN_PASTE_CHUNK_CAPACITY 128u

typedef enum kittyin_event_kind {
    KITTYIN_EVENT_KEY = 1,
    KITTYIN_EVENT_MOUSE = 2,
    KITTYIN_EVENT_FOCUS = 3,
    KITTYIN_EVENT_PASTE = 4
} kittyin_event_kind;

typedef enum kittyin_mouse_action {
    KITTYIN_MOUSE_PRESS = 1,
    KITTYIN_MOUSE_RELEASE = 2,
    KITTYIN_MOUSE_MOVE = 3,
    KITTYIN_MOUSE_WHEEL = 4
} kittyin_mouse_action;

typedef struct kittyin_mouse_event {
    int32_t x;
    int32_t y;
    int8_t wheel_x;
    int8_t wheel_y;
    uint32_t modifiers;
    uint32_t raw_button;
    uint8_t button;
    uint8_t action;
    bool pixel_coordinates;
} kittyin_mouse_event;

typedef struct kittyin_focus_event {
    bool focused;
} kittyin_focus_event;

typedef struct kittyin_paste_event {
    unsigned char bytes[KITTYIN_PASTE_CHUNK_CAPACITY];
    size_t length;
    bool final;
} kittyin_paste_event;

typedef struct kittyin_event {
    kittyin_event_kind kind;
    union {
        kittykb_event key;
        kittyin_mouse_event mouse;
        kittyin_focus_event focus;
        kittyin_paste_event paste;
    } data;
} kittyin_event;

typedef struct kittyin_stats {
    uint64_t bytes_fed;
    uint64_t events_decoded;
    uint64_t dropped_events;
    uint64_t malformed_sequences;
    uint64_t ignored_sequences;
    uint64_t paste_chunks;
} kittyin_stats;

/* Public so callers can allocate without malloc. Fields are internal. */
typedef struct kittyin_input {
    kittyin_event queue[KITTYIN_EVENT_QUEUE_CAPACITY];
    kittykb_input keyboard_storage;
    kittykb_input *keyboard;
    char sequence[KITTYIN_SEQUENCE_CAPACITY];
    unsigned char paste[KITTYIN_PASTE_CHUNK_CAPACITY];
    kittyin_stats stats;
    size_t queue_head;
    size_t queue_count;
    size_t sequence_length;
    size_t paste_length;
    size_t paste_match_length;
    uint8_t parser_state;
    bool pixel_coordinates;
} kittyin_input;

void kittyin_input_init(kittyin_input *input);

/* Advanced: bind the parser to a lifecycle owner's existing keyboard state. */
void kittyin_input_bind_keyboard(kittyin_input *input, kittykb_input *keyboard);
kittykb_input *kittyin_input_keyboard(kittyin_input *input);
const kittykb_input *kittyin_input_keyboard_const(const kittyin_input *input);

void kittyin_input_set_pixel_coordinates(kittyin_input *input, bool enabled);

/* Feed arbitrary fragments. A zero-length feed drains pre-existing key events. */
void kittyin_input_feed(kittyin_input *input, const void *bytes,
                        size_t byte_count);
void kittyin_input_flush_escape(kittyin_input *input);
bool kittyin_input_has_pending_escape(const kittyin_input *input);

bool kittyin_input_next(kittyin_input *input, kittyin_event *event);
void kittyin_input_clear_events(kittyin_input *input);
void kittyin_input_release_all(kittyin_input *input);

bool kittyin_input_key_down(const kittyin_input *input, uint32_t key);
bool kittyin_input_has_release_events(const kittyin_input *input);
size_t kittyin_input_held_count(const kittyin_input *input);

const kittyin_stats *kittyin_input_get_stats(const kittyin_input *input);

#ifdef __cplusplus
}
#endif

#endif
