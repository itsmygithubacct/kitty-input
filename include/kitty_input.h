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
#define KITTYIN_VERSION_MINOR 2
#define KITTYIN_VERSION_PATCH 1

#define KITTYIN_EVENT_QUEUE_CAPACITY 128u
#define KITTYIN_SEQUENCE_CAPACITY 256u
#define KITTYIN_PASTE_CHUNK_CAPACITY 128u
#define KITTYIN_ACTION_CAPACITY 64u
#define KITTYIN_ACTION_BINDING_CAPACITY 96u
#define KITTYIN_GAMEPAD_CAPACITY 4u
#define KITTYIN_GAMEPAD_ANY UINT8_MAX

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

typedef enum kittyin_device_kind {
    KITTYIN_DEVICE_KEYBOARD = 1,
    KITTYIN_DEVICE_GAMEPAD = 2
} kittyin_device_kind;

typedef enum kittyin_gamepad_event_kind {
    KITTYIN_GAMEPAD_CONNECTED = 1,
    KITTYIN_GAMEPAD_DISCONNECTED = 2,
    KITTYIN_GAMEPAD_BUTTON = 3,
    KITTYIN_GAMEPAD_AXIS = 4
} kittyin_gamepad_event_kind;

typedef struct kittyin_gamepad_event {
    kittyin_gamepad_event_kind kind;
    uint8_t gamepad;
    uint8_t control;
    int16_t value;
    bool initial;
} kittyin_gamepad_event;

typedef enum kittyin_action_phase {
    KITTYIN_ACTION_PRESS = 1,
    KITTYIN_ACTION_REPEAT = 2,
    KITTYIN_ACTION_RELEASE = 3
} kittyin_action_phase;

typedef struct kittyin_action_event {
    uint16_t action;
    kittyin_action_phase phase;
    kittyin_device_kind device;
    uint8_t gamepad;
} kittyin_action_event;

typedef enum kittyin_action_binding_kind {
    KITTYIN_BINDING_KEY = 1,
    KITTYIN_BINDING_GAMEPAD_BUTTON = 2,
    KITTYIN_BINDING_GAMEPAD_AXIS = 3
} kittyin_action_binding_kind;

typedef struct kittyin_action_binding {
    uint32_t key;
    uint32_t modifiers;
    uint16_t action;
    uint8_t kind;
    uint8_t gamepad;
    uint8_t control;
    int8_t direction;
    uint8_t down_gamepads;
} kittyin_action_binding;

typedef struct kittyin_action_state {
    uint32_t held_ms;
    uint32_t next_repeat_ms;
    uint16_t sources;
    uint8_t gamepad;
} kittyin_action_state;

/*
 * Public so callers can allocate without malloc. Fields are internal.
 * Action zero is valid; consumers define the meaning of all action IDs.
 */
typedef struct kittyin_action_map {
    kittyin_action_binding bindings[KITTYIN_ACTION_BINDING_CAPACITY];
    kittyin_action_state actions[KITTYIN_ACTION_CAPACITY];
    size_t binding_count;
    uint32_t repeat_delay_ms;
    uint32_t repeat_interval_ms;
    int16_t axis_dead_zone;
    uint8_t connected_gamepads;
    uint8_t active_gamepad;
    uint8_t active_device;
} kittyin_action_map;

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

/*
 * Semantic action mapping. Keyboard repeat events pass through unchanged.
 * Gamepad buttons and axes use advance() for deterministic held repeat.
 * Output functions return at most capacity stored events; state transitions
 * still occur after the output buffer fills or when capacity is zero.
 */
void kittyin_action_map_init(kittyin_action_map *map);
bool kittyin_action_map_bind_key(kittyin_action_map *map, uint16_t action,
                                 uint32_t key, uint32_t modifiers);
bool kittyin_action_map_bind_gamepad_button(kittyin_action_map *map,
                                            uint16_t action,
                                            uint8_t gamepad,
                                            uint8_t button);
bool kittyin_action_map_bind_gamepad_axis(kittyin_action_map *map,
                                          uint16_t action,
                                          uint8_t gamepad, uint8_t axis,
                                          int direction);
size_t kittyin_action_map_unbind(kittyin_action_map *map, uint16_t action,
                                 kittyin_device_kind device);
bool kittyin_action_map_has_binding(const kittyin_action_map *map,
                                    uint16_t action,
                                    kittyin_device_kind device);
bool kittyin_action_map_set_repeat(kittyin_action_map *map,
                                   uint32_t delay_ms,
                                   uint32_t interval_ms);
bool kittyin_action_map_set_dead_zone(kittyin_action_map *map,
                                      int dead_zone);
size_t kittyin_action_map_key(kittyin_action_map *map,
                              const kittykb_event *key,
                              kittyin_action_event *events,
                              size_t capacity);
size_t kittyin_action_map_gamepad(kittyin_action_map *map,
                                  const kittyin_gamepad_event *gamepad,
                                  kittyin_action_event *events,
                                  size_t capacity);
/* Coalesces missed intervals and emits at most one repeat per held action. */
size_t kittyin_action_map_advance(kittyin_action_map *map,
                                  uint32_t elapsed_ms,
                                  kittyin_action_event *events,
                                  size_t capacity);
size_t kittyin_action_map_release_all(kittyin_action_map *map,
                                      kittyin_action_event *events,
                                      size_t capacity);
kittyin_device_kind
kittyin_action_map_active_device(const kittyin_action_map *map);
uint8_t kittyin_action_map_active_gamepad(const kittyin_action_map *map);

#ifdef __cplusplus
}
#endif

#endif
