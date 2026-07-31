#ifndef KITTY_KEYBOARD_H
#define KITTY_KEYBOARD_H

/*
 * A small, allocation-free decoder and held-key state tracker for the Kitty
 * keyboard protocol.  Key values use Kitty's canonical Unicode/PUA numbers.
 *
 * The parser is portable ISO C.  Terminal setup and non-blocking reads live in
 * kitty_keyboard_posix.h so applications that already own termios can use this
 * file on its own.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KITTYKB_VERSION_MAJOR 0
#define KITTYKB_VERSION_MINOR 1
#define KITTYKB_VERSION_PATCH 0

#define KITTYKB_MAX_TEXT_CODEPOINTS 16u
#define KITTYKB_EVENT_QUEUE_CAPACITY 128u
#define KITTYKB_HELD_KEY_CAPACITY 128u
#define KITTYKB_SEQUENCE_CAPACITY 256u

/* Progressive enhancement flags from the Kitty keyboard protocol. */
enum kittykb_flag {
    KITTYKB_FLAG_DISAMBIGUATE = 1u << 0,
    KITTYKB_FLAG_REPORT_EVENTS = 1u << 1,
    KITTYKB_FLAG_REPORT_ALTERNATES = 1u << 2,
    KITTYKB_FLAG_REPORT_ALL_KEYS = 1u << 3,
    KITTYKB_FLAG_REPORT_TEXT = 1u << 4,

    /* Recommended flags for reliable key-state tracking. */
    KITTYKB_FLAGS_KEY_STATE = KITTYKB_FLAG_DISAMBIGUATE |
                              KITTYKB_FLAG_REPORT_EVENTS |
                              KITTYKB_FLAG_REPORT_ALTERNATES |
                              KITTYKB_FLAG_REPORT_ALL_KEYS,

    /* Also preserve IME/layout-produced text for text fields and consoles. */
    KITTYKB_FLAGS_DEFAULT = KITTYKB_FLAGS_KEY_STATE | KITTYKB_FLAG_REPORT_TEXT
};

/* The wire stores 1 + this bitset. */
enum kittykb_modifier {
    KITTYKB_MOD_SHIFT = 1u << 0,
    KITTYKB_MOD_ALT = 1u << 1,
    KITTYKB_MOD_CTRL = 1u << 2,
    KITTYKB_MOD_SUPER = 1u << 3,
    KITTYKB_MOD_HYPER = 1u << 4,
    KITTYKB_MOD_META = 1u << 5,
    KITTYKB_MOD_CAPS_LOCK = 1u << 6,
    KITTYKB_MOD_NUM_LOCK = 1u << 7
};

/* Kitty functional key values.  Ordinary keys are Unicode scalar values. */
enum kittykb_key {
    KITTYKB_KEY_NONE = 0,
    KITTYKB_KEY_ESCAPE = 0xe000,
    KITTYKB_KEY_ENTER = 0xe001,
    KITTYKB_KEY_TAB = 0xe002,
    KITTYKB_KEY_BACKSPACE = 0xe003,
    KITTYKB_KEY_INSERT = 0xe004,
    KITTYKB_KEY_DELETE = 0xe005,
    KITTYKB_KEY_LEFT = 0xe006,
    KITTYKB_KEY_RIGHT = 0xe007,
    KITTYKB_KEY_UP = 0xe008,
    KITTYKB_KEY_DOWN = 0xe009,
    KITTYKB_KEY_PAGE_UP = 0xe00a,
    KITTYKB_KEY_PAGE_DOWN = 0xe00b,
    KITTYKB_KEY_HOME = 0xe00c,
    KITTYKB_KEY_END = 0xe00d,
    KITTYKB_KEY_CAPS_LOCK = 0xe00e,
    KITTYKB_KEY_SCROLL_LOCK = 0xe00f,
    KITTYKB_KEY_NUM_LOCK = 0xe010,
    KITTYKB_KEY_PRINT_SCREEN = 0xe011,
    KITTYKB_KEY_PAUSE = 0xe012,
    KITTYKB_KEY_MENU = 0xe013,
    KITTYKB_KEY_F1 = 0xe014,
    KITTYKB_KEY_F2 = 0xe015,
    KITTYKB_KEY_F3 = 0xe016,
    KITTYKB_KEY_F4 = 0xe017,
    KITTYKB_KEY_F5 = 0xe018,
    KITTYKB_KEY_F6 = 0xe019,
    KITTYKB_KEY_F7 = 0xe01a,
    KITTYKB_KEY_F8 = 0xe01b,
    KITTYKB_KEY_F9 = 0xe01c,
    KITTYKB_KEY_F10 = 0xe01d,
    KITTYKB_KEY_F11 = 0xe01e,
    KITTYKB_KEY_F12 = 0xe01f,
    KITTYKB_KEY_F13 = 0xe020,
    KITTYKB_KEY_F14 = 0xe021,
    KITTYKB_KEY_F15 = 0xe022,
    KITTYKB_KEY_F16 = 0xe023,
    KITTYKB_KEY_F17 = 0xe024,
    KITTYKB_KEY_F18 = 0xe025,
    KITTYKB_KEY_F19 = 0xe026,
    KITTYKB_KEY_F20 = 0xe027,
    KITTYKB_KEY_F21 = 0xe028,
    KITTYKB_KEY_F22 = 0xe029,
    KITTYKB_KEY_F23 = 0xe02a,
    KITTYKB_KEY_F24 = 0xe02b,
    KITTYKB_KEY_F25 = 0xe02c,
    KITTYKB_KEY_F26 = 0xe02d,
    KITTYKB_KEY_F27 = 0xe02e,
    KITTYKB_KEY_F28 = 0xe02f,
    KITTYKB_KEY_F29 = 0xe030,
    KITTYKB_KEY_F30 = 0xe031,
    KITTYKB_KEY_F31 = 0xe032,
    KITTYKB_KEY_F32 = 0xe033,
    KITTYKB_KEY_F33 = 0xe034,
    KITTYKB_KEY_F34 = 0xe035,
    KITTYKB_KEY_F35 = 0xe036,
    KITTYKB_KEY_KP_0 = 0xe037,
    KITTYKB_KEY_KP_1 = 0xe038,
    KITTYKB_KEY_KP_2 = 0xe039,
    KITTYKB_KEY_KP_3 = 0xe03a,
    KITTYKB_KEY_KP_4 = 0xe03b,
    KITTYKB_KEY_KP_5 = 0xe03c,
    KITTYKB_KEY_KP_6 = 0xe03d,
    KITTYKB_KEY_KP_7 = 0xe03e,
    KITTYKB_KEY_KP_8 = 0xe03f,
    KITTYKB_KEY_KP_9 = 0xe040,
    KITTYKB_KEY_KP_DECIMAL = 0xe041,
    KITTYKB_KEY_KP_DIVIDE = 0xe042,
    KITTYKB_KEY_KP_MULTIPLY = 0xe043,
    KITTYKB_KEY_KP_SUBTRACT = 0xe044,
    KITTYKB_KEY_KP_ADD = 0xe045,
    KITTYKB_KEY_KP_ENTER = 0xe046,
    KITTYKB_KEY_KP_EQUAL = 0xe047,
    KITTYKB_KEY_KP_SEPARATOR = 0xe048,
    KITTYKB_KEY_KP_LEFT = 0xe049,
    KITTYKB_KEY_KP_RIGHT = 0xe04a,
    KITTYKB_KEY_KP_UP = 0xe04b,
    KITTYKB_KEY_KP_DOWN = 0xe04c,
    KITTYKB_KEY_KP_PAGE_UP = 0xe04d,
    KITTYKB_KEY_KP_PAGE_DOWN = 0xe04e,
    KITTYKB_KEY_KP_HOME = 0xe04f,
    KITTYKB_KEY_KP_END = 0xe050,
    KITTYKB_KEY_KP_INSERT = 0xe051,
    KITTYKB_KEY_KP_DELETE = 0xe052,
    KITTYKB_KEY_KP_BEGIN = 0xe053,
    KITTYKB_KEY_MEDIA_PLAY = 0xe054,
    KITTYKB_KEY_MEDIA_PAUSE = 0xe055,
    KITTYKB_KEY_MEDIA_PLAY_PAUSE = 0xe056,
    KITTYKB_KEY_MEDIA_REVERSE = 0xe057,
    KITTYKB_KEY_MEDIA_STOP = 0xe058,
    KITTYKB_KEY_MEDIA_FAST_FORWARD = 0xe059,
    KITTYKB_KEY_MEDIA_REWIND = 0xe05a,
    KITTYKB_KEY_MEDIA_TRACK_NEXT = 0xe05b,
    KITTYKB_KEY_MEDIA_TRACK_PREVIOUS = 0xe05c,
    KITTYKB_KEY_MEDIA_RECORD = 0xe05d,
    KITTYKB_KEY_LOWER_VOLUME = 0xe05e,
    KITTYKB_KEY_RAISE_VOLUME = 0xe05f,
    KITTYKB_KEY_MUTE_VOLUME = 0xe060,
    KITTYKB_KEY_LEFT_SHIFT = 0xe061,
    KITTYKB_KEY_LEFT_CONTROL = 0xe062,
    KITTYKB_KEY_LEFT_ALT = 0xe063,
    KITTYKB_KEY_LEFT_SUPER = 0xe064,
    KITTYKB_KEY_LEFT_HYPER = 0xe065,
    KITTYKB_KEY_LEFT_META = 0xe066,
    KITTYKB_KEY_RIGHT_SHIFT = 0xe067,
    KITTYKB_KEY_RIGHT_CONTROL = 0xe068,
    KITTYKB_KEY_RIGHT_ALT = 0xe069,
    KITTYKB_KEY_RIGHT_SUPER = 0xe06a,
    KITTYKB_KEY_RIGHT_HYPER = 0xe06b,
    KITTYKB_KEY_RIGHT_META = 0xe06c,
    KITTYKB_KEY_ISO_LEVEL3_SHIFT = 0xe06d,
    KITTYKB_KEY_ISO_LEVEL5_SHIFT = 0xe06e
};

typedef enum kittykb_action {
    KITTYKB_ACTION_PRESS = 1,
    KITTYKB_ACTION_REPEAT = 2,
    KITTYKB_ACTION_RELEASE = 3
} kittykb_action;

typedef enum kittykb_event_source {
    KITTYKB_SOURCE_LEGACY = 0,
    KITTYKB_SOURCE_KITTY = 1
} kittykb_event_source;

typedef enum kittykb_support {
    KITTYKB_SUPPORT_UNKNOWN = 0,
    KITTYKB_SUPPORT_UNAVAILABLE = 1,
    KITTYKB_SUPPORT_AVAILABLE = 2
} kittykb_support;

typedef struct kittykb_event {
    uint32_t key;
    uint32_t shifted_key;
    uint32_t base_layout_key;
    uint32_t modifiers;
    kittykb_action action;
    uint32_t text[KITTYKB_MAX_TEXT_CODEPOINTS];
    uint8_t text_length;
    uint8_t source;
    bool event_type_explicit;
} kittykb_event;

typedef struct kittykb_stats {
    uint64_t bytes_read;
    uint64_t events_decoded;
    uint64_t malformed_sequences;
    uint64_t ignored_sequences;
    uint64_t dropped_events;
    uint64_t dropped_held_keys;
} kittykb_stats;

typedef struct kittykb_held_key {
    uint32_t key;
    uint32_t shifted_key;
    uint32_t base_layout_key;
    uint32_t modifiers;
} kittykb_held_key;

/* Public so callers can allocate it without malloc; fields are internal. */
typedef struct kittykb_input {
    kittykb_event queue[KITTYKB_EVENT_QUEUE_CAPACITY];
    kittykb_held_key held[KITTYKB_HELD_KEY_CAPACITY];
    char sequence[KITTYKB_SEQUENCE_CAPACITY];
    kittykb_stats stats;
    uint32_t requested_flags;
    uint32_t terminal_flags;
    uint32_t utf8_value;
    uint32_t utf8_minimum;
    uint32_t utf8_modifiers;
    size_t queue_head;
    size_t queue_count;
    size_t held_count;
    size_t sequence_length;
    uint8_t parser_state;
    uint8_t string_kind;
    uint8_t utf8_remaining;
    uint8_t support;
    bool saw_primary_device_attributes;
    bool observed_explicit_release;
} kittykb_input;

void kittykb_input_init(kittykb_input *input);
void kittykb_input_set_requested_flags(kittykb_input *input, uint32_t flags);

/* Feed arbitrary fragments; escape sequences may span any number of calls. */
void kittykb_input_feed(
    kittykb_input *input,
    const void *bytes,
    size_t byte_count);

/* Resolve a pending lone ESC after the caller's legacy escape timeout. */
void kittykb_input_flush_escape(kittykb_input *input);
bool kittykb_input_has_pending_escape(const kittykb_input *input);

bool kittykb_input_next(kittykb_input *input, kittykb_event *event);
void kittykb_input_clear_events(kittykb_input *input);

/*
 * Held state is trustworthy only when this returns true.  key_down() returns
 * false otherwise, preventing legacy press-only input from becoming stuck.
 */
bool kittykb_input_has_release_events(const kittykb_input *input);
bool kittykb_input_key_down(const kittykb_input *input, uint32_t key);
size_t kittykb_input_held_count(const kittykb_input *input);
void kittykb_input_release_all(kittykb_input *input);

kittykb_support kittykb_input_protocol_support(const kittykb_input *input);
uint32_t kittykb_input_terminal_flags(const kittykb_input *input);
void kittykb_input_set_terminal_flags(kittykb_input *input, uint32_t flags);
void kittykb_input_finish_probe(kittykb_input *input);

const kittykb_stats *kittykb_input_get_stats(const kittykb_input *input);

/* Match the logical key, its shifted spelling, or its PC-101 base-layout key. */
bool kittykb_event_matches_key(const kittykb_event *event, uint32_t key);

/* Returns NULL for ordinary Unicode keys and unknown values. */
const char *kittykb_key_name(uint32_t key);

#ifdef __cplusplus
}
#endif

#endif
