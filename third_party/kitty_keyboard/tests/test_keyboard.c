#include "kitty_keyboard.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(                                                    \
                stderr,                                                       \
                "%s:%d: check failed: %s\n",                               \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #condition);                                                  \
            return false;                                                     \
        }                                                                     \
    } while (false)

static bool
next_event(kittykb_input *input, kittykb_event *event)
{
    CHECK(kittykb_input_next(input, event));
    return true;
}

static void
feed_fragmented(kittykb_input *input, const char *text)
{
    size_t index;

    for (index = 0u; text[index] != '\0'; ++index) {
        kittykb_input_feed(input, &text[index], 1u);
    }
}

static bool
test_two_simultaneous_holds(void)
{
    kittykb_input input;
    kittykb_event event;

    kittykb_input_init(&input);
    feed_fragmented(&input, "\x1b[?31u");
    CHECK(kittykb_input_protocol_support(&input) ==
          KITTYKB_SUPPORT_AVAILABLE);
    CHECK(kittykb_input_has_release_events(&input));

    feed_fragmented(
        &input,
        "\x1b[119;;119u"    /* W press + associated text */
        "\x1b[100;;100u"    /* D press + associated text */
        "\x1b[119;1:2;119u"); /* W repeat */
    CHECK(kittykb_input_key_down(&input, (uint32_t)'w'));
    CHECK(kittykb_input_key_down(&input, (uint32_t)'d'));
    CHECK(kittykb_input_held_count(&input) == 2u);

    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'w');
    CHECK(event.action == KITTYKB_ACTION_PRESS);
    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'d');
    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'w');
    CHECK(event.action == KITTYKB_ACTION_REPEAT);

    feed_fragmented(&input, "\x1b[119;1:3u");
    CHECK(!kittykb_input_key_down(&input, (uint32_t)'w'));
    CHECK(kittykb_input_key_down(&input, (uint32_t)'d'));
    CHECK(kittykb_input_held_count(&input) == 1u);

    feed_fragmented(&input, "\x1b[100;1:3u");
    CHECK(!kittykb_input_key_down(&input, (uint32_t)'d'));
    CHECK(kittykb_input_held_count(&input) == 0u);
    return true;
}

static bool
test_alternates_modifiers_and_text(void)
{
    kittykb_input input;
    kittykb_event event;

    kittykb_input_init(&input);
    kittykb_input_set_terminal_flags(&input, KITTYKB_FLAGS_DEFAULT);
    kittykb_input_feed(
        &input,
        "\x1b[97:65:99;6:1;65u",
        sizeof("\x1b[97:65:99;6:1;65u") - 1u);
    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'a');
    CHECK(event.shifted_key == (uint32_t)'A');
    CHECK(event.base_layout_key == (uint32_t)'c');
    CHECK(event.modifiers == (KITTYKB_MOD_SHIFT | KITTYKB_MOD_CTRL));
    CHECK(event.action == KITTYKB_ACTION_PRESS);
    CHECK(event.event_type_explicit);
    CHECK(event.source == (uint8_t)KITTYKB_SOURCE_KITTY);
    CHECK(event.text_length == 1u && event.text[0] == (uint32_t)'A');
    CHECK(kittykb_event_matches_key(&event, (uint32_t)'a'));
    CHECK(kittykb_event_matches_key(&event, (uint32_t)'A'));
    CHECK(kittykb_event_matches_key(&event, (uint32_t)'c'));
    return true;
}

static bool
test_text_only_event(void)
{
    kittykb_input input;
    kittykb_event event;

    kittykb_input_init(&input);
    kittykb_input_feed(
        &input,
        "\x1b[0;;229u",
        sizeof("\x1b[0;;229u") - 1u);
    CHECK(next_event(&input, &event));
    CHECK(event.key == KITTYKB_KEY_NONE);
    CHECK(event.text_length == 1u);
    CHECK(event.text[0] == 229u);
    CHECK(!kittykb_event_matches_key(&event, KITTYKB_KEY_NONE));
    CHECK(!kittykb_input_key_down(&input, KITTYKB_KEY_NONE));
    CHECK(kittykb_input_held_count(&input) == 0u);
    return true;
}

static bool
test_functional_keys(void)
{
    static const char input_bytes[] =
        "\x1b[?15u"
        "\x1b[A"
        "\x1b[1;1:3A"
        "\x1b[27;1:1u"
        "\x1bOP"
        "\x1bOR"
        "\x1b[13~"
        "\x1b[1;2Z"
        "\x1b[R"; /* CPR trailer is not the obsolete CSI-R F3 form. */
    const uint32_t expected[] = {
        KITTYKB_KEY_UP,
        KITTYKB_KEY_UP,
        KITTYKB_KEY_ESCAPE,
        KITTYKB_KEY_F1,
        KITTYKB_KEY_F3,
        KITTYKB_KEY_F3,
        KITTYKB_KEY_TAB
    };
    kittykb_input input;
    kittykb_event event;
    size_t index;

    kittykb_input_init(&input);
    feed_fragmented(&input, input_bytes);
    for (index = 0u; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        CHECK(next_event(&input, &event));
        CHECK(event.key == expected[index]);
        if (index == 1u) {
            CHECK(event.action == KITTYKB_ACTION_RELEASE);
        }
    }
    CHECK((event.modifiers & KITTYKB_MOD_SHIFT) != 0u);
    CHECK(!kittykb_input_next(&input, &event));
    CHECK(kittykb_input_get_stats(&input)->ignored_sequences == 1u);
    CHECK(!kittykb_input_key_down(&input, KITTYKB_KEY_UP));
    return true;
}

static bool
test_legacy_functional_matrix(void)
{
    static const struct {
        const char *sequence;
        uint32_t key;
    } cases[] = {
        {"\x1b[2~", KITTYKB_KEY_INSERT},
        {"\x1b[3~", KITTYKB_KEY_DELETE},
        {"\x1b[5~", KITTYKB_KEY_PAGE_UP},
        {"\x1b[6~", KITTYKB_KEY_PAGE_DOWN},
        {"\x1b[7~", KITTYKB_KEY_HOME},
        {"\x1b[8~", KITTYKB_KEY_END},
        {"\x1b[15~", KITTYKB_KEY_F5},
        {"\x1b[17~", KITTYKB_KEY_F6},
        {"\x1b[18~", KITTYKB_KEY_F7},
        {"\x1b[19~", KITTYKB_KEY_F8},
        {"\x1b[20~", KITTYKB_KEY_F9},
        {"\x1b[21~", KITTYKB_KEY_F10},
        {"\x1b[23~", KITTYKB_KEY_F11},
        {"\x1b[24~", KITTYKB_KEY_F12},
        {"\x1b[29~", KITTYKB_KEY_MENU},
        {"\x1b[H", KITTYKB_KEY_HOME},
        {"\x1b[F", KITTYKB_KEY_END},
        {"\x1bOE", KITTYKB_KEY_KP_BEGIN},
        {"\x1bOS", KITTYKB_KEY_F4}
    };
    kittykb_input input;
    kittykb_event event;
    size_t index;

    kittykb_input_init(&input);
    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        feed_fragmented(&input, cases[index].sequence);
        CHECK(next_event(&input, &event));
        CHECK(event.key == cases[index].key);
        CHECK(!kittykb_input_next(&input, &event));
    }

    feed_fragmented(&input, "\x1b[1;6D");
    CHECK(next_event(&input, &event));
    CHECK(event.key == KITTYKB_KEY_LEFT);
    CHECK(event.modifiers == (KITTYKB_MOD_SHIFT | KITTYKB_MOD_CTRL));
    return true;
}

static bool
test_legacy_utf8_alt_and_escape(void)
{
    static const unsigned char utf8[] = {0xc3u, 0xa9u};
    kittykb_input input;
    kittykb_event event;

    kittykb_input_init(&input);
    kittykb_input_feed(&input, utf8, 1u);
    CHECK(!kittykb_input_next(&input, &event));
    kittykb_input_feed(&input, utf8 + 1u, 1u);
    CHECK(next_event(&input, &event));
    CHECK(event.key == 0xe9u && event.text[0] == 0xe9u);
    CHECK(!kittykb_input_key_down(&input, 0xe9u));

    kittykb_input_feed(&input, "\x1bx", 2u);
    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'x');
    CHECK(event.modifiers == KITTYKB_MOD_ALT);

    kittykb_input_feed(&input, "\x1b", 1u);
    CHECK(kittykb_input_has_pending_escape(&input));
    CHECK(!kittykb_input_next(&input, &event));
    kittykb_input_flush_escape(&input);
    CHECK(next_event(&input, &event));
    CHECK(event.key == KITTYKB_KEY_ESCAPE);
    CHECK(event.source == (uint8_t)KITTYKB_SOURCE_LEGACY);
    return true;
}

static bool
test_responses_and_control_strings_are_filtered(void)
{
    static const char bytes[] =
        "\x1b_Gi=123;OK\x1b\\"
        "\x1b]window title\x07"
        "\x1bPignored\x1b\\"
        "\x1b[?31u"
        "\x1b[?1;2c"
        "\x1b[113;1:1u";
    kittykb_input input;
    kittykb_event event;

    kittykb_input_init(&input);
    kittykb_input_feed(&input, bytes, sizeof(bytes) - 1u);
    CHECK(kittykb_input_protocol_support(&input) ==
          KITTYKB_SUPPORT_AVAILABLE);
    CHECK(kittykb_input_terminal_flags(&input) == 31u);
    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'q');
    CHECK(!kittykb_input_next(&input, &event));
    CHECK(kittykb_input_get_stats(&input)->ignored_sequences == 3u);
    CHECK(kittykb_input_get_stats(&input)->malformed_sequences == 0u);
    return true;
}

static bool
test_capability_detection_order(void)
{
    kittykb_input input;

    kittykb_input_init(&input);
    feed_fragmented(&input, "\x1b[?1;2c");
    CHECK(kittykb_input_protocol_support(&input) ==
          KITTYKB_SUPPORT_UNAVAILABLE);
    CHECK(!kittykb_input_has_release_events(&input));

    /* A delayed status response still upgrades the result safely. */
    feed_fragmented(&input, "\x1b[?31u");
    CHECK(kittykb_input_protocol_support(&input) ==
          KITTYKB_SUPPORT_AVAILABLE);
    CHECK(kittykb_input_has_release_events(&input));
    return true;
}

static bool
test_base_layout_held_alias(void)
{
    kittykb_input input;

    kittykb_input_init(&input);
    kittykb_input_set_terminal_flags(&input, KITTYKB_FLAGS_KEY_STATE);
    feed_fragmented(&input, "\x1b[1089::119;1:1u");
    CHECK(kittykb_input_key_down(&input, 1089u));
    CHECK(kittykb_input_key_down(&input, (uint32_t)'w'));
    feed_fragmented(&input, "\x1b[1089::119;1:3u");
    CHECK(!kittykb_input_key_down(&input, 1089u));
    CHECK(!kittykb_input_key_down(&input, (uint32_t)'w'));
    return true;
}

static bool
test_insufficient_flags_never_claim_held_state(void)
{
    kittykb_input input;

    kittykb_input_init(&input);
    kittykb_input_set_terminal_flags(&input, KITTYKB_FLAG_REPORT_EVENTS);
    feed_fragmented(&input, "\x1b[119;1:1u");
    CHECK(!kittykb_input_has_release_events(&input));
    CHECK(!kittykb_input_key_down(&input, (uint32_t)'w'));
    CHECK(kittykb_input_held_count(&input) == 0u);
    return true;
}

static bool
test_overlong_sequence_recovers(void)
{
    kittykb_input input;
    kittykb_event event;
    char byte = '1';
    size_t index;

    kittykb_input_init(&input);
    kittykb_input_feed(&input, "\x1b[", 2u);
    for (index = 0u; index < KITTYKB_SEQUENCE_CAPACITY + 20u; ++index) {
        kittykb_input_feed(&input, &byte, 1u);
    }
    kittykb_input_feed(&input, "u", 1u);
    feed_fragmented(&input, "\x1b[113;1:1u");
    CHECK(next_event(&input, &event));
    CHECK(event.key == (uint32_t)'q');
    CHECK(!kittykb_input_next(&input, &event));
    CHECK(kittykb_input_get_stats(&input)->malformed_sequences == 1u);
    return true;
}

static bool
test_queue_overflow_keeps_state_correct(void)
{
    kittykb_input input;
    size_t index;

    kittykb_input_init(&input);
    kittykb_input_set_terminal_flags(&input, KITTYKB_FLAGS_KEY_STATE);
    for (index = 0u; index < KITTYKB_EVENT_QUEUE_CAPACITY + 25u; ++index) {
        feed_fragmented(&input, "\x1b[119;1:2u");
    }
    CHECK(kittykb_input_key_down(&input, (uint32_t)'w'));
    CHECK(kittykb_input_held_count(&input) == 1u);
    CHECK(kittykb_input_get_stats(&input)->dropped_events == 25u);
    feed_fragmented(&input, "\x1b[119;1:3u");
    CHECK(!kittykb_input_key_down(&input, (uint32_t)'w'));
    return true;
}

static bool
test_functional_key_names(void)
{
    uint32_t key;

    for (key = KITTYKB_KEY_ESCAPE;
         key <= (uint32_t)KITTYKB_KEY_ISO_LEVEL5_SHIFT;
         ++key) {
        CHECK(kittykb_key_name(key) != NULL);
    }
    CHECK(kittykb_key_name((uint32_t)'w') == NULL);
    CHECK(kittykb_key_name(KITTYKB_KEY_NONE) == NULL);
    return true;
}

static bool
contains_bytes(
    const char *haystack,
    size_t haystack_size,
    const char *needle,
    size_t needle_size)
{
    size_t index;

    if (needle_size > haystack_size) {
        return false;
    }
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) {
            return true;
        }
    }
    return false;
}

static size_t
read_available(int fd, char *output, size_t capacity)
{
    size_t total = 0u;

    while (total < capacity) {
        const ssize_t count = read(fd, output + total, capacity - total);

        if (count > 0) {
            total += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    return total;
}

static bool
wait_readable(int fd)
{
    struct pollfd descriptor;
    int result;

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    do {
        result = poll(&descriptor, 1u, 200);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & POLLIN) != 0;
}

static bool
test_posix_terminal_lifecycle(void)
{
    static const char response_and_keys[] =
        "\x1b[?31u"
        "\x1b[?1;2c"
        "\x1b[119;1:1u"
        "\x1b[100;1:1u";
    int master = -1;
    int slave = -1;
    int master_flags;
    struct termios original;
    struct termios active;
    struct termios restored;
    kittykb_terminal terminal;
    kittykb_terminal_options options;
    char output[256];
    size_t output_size;

    CHECK(openpty(&master, &slave, NULL, NULL, NULL) == 0);
    CHECK(tcgetattr(slave, &original) == 0);
    master_flags = fcntl(master, F_GETFL);
    CHECK(master_flags >= 0);
    CHECK(fcntl(master, F_SETFL, master_flags | O_NONBLOCK) == 0);

    kittykb_terminal_init(&terminal);
    kittykb_terminal_options_init(&options);
    options.probe_timeout_ms = 0;
    CHECK(kittykb_terminal_start(&terminal, slave, slave, &options) == 0);
    CHECK(tcgetattr(slave, &active) == 0);
    CHECK((active.c_lflag & (ECHO | ICANON | ISIG)) == 0u);

    CHECK(wait_readable(master));
    output_size = read_available(master, output, sizeof(output));
    CHECK(contains_bytes(output, output_size, "\x1b[>31u", 6u));
    CHECK(contains_bytes(output, output_size, "\x1b[?u", 4u));
    CHECK(contains_bytes(output, output_size, "\x1b[c", 3u));

    CHECK(write(
              master,
              response_and_keys,
              sizeof(response_and_keys) - 1u) ==
          (ssize_t)(sizeof(response_and_keys) - 1u));
    CHECK(wait_readable(slave));
    CHECK(kittykb_terminal_read(&terminal) > 0);
    CHECK(kittykb_input_protocol_support(&terminal.input) ==
          KITTYKB_SUPPORT_AVAILABLE);
    CHECK(kittykb_input_key_down(&terminal.input, (uint32_t)'w'));
    CHECK(kittykb_input_key_down(&terminal.input, (uint32_t)'d'));

    CHECK(kittykb_terminal_stop(&terminal) == 0);
    CHECK(tcgetattr(slave, &restored) == 0);
    CHECK(restored.c_iflag == original.c_iflag);
    CHECK(restored.c_oflag == original.c_oflag);
    CHECK(restored.c_cflag == original.c_cflag);
    CHECK(restored.c_lflag == original.c_lflag);
    CHECK(wait_readable(master));
    output_size = read_available(master, output, sizeof(output));
    CHECK(contains_bytes(output, output_size, "\x1b[<u", 4u));
    CHECK(kittykb_terminal_stop(&terminal) == 0);

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"two simultaneous holds", test_two_simultaneous_holds},
        {"alternates, modifiers, and text", test_alternates_modifiers_and_text},
        {"text-only event", test_text_only_event},
        {"functional keys", test_functional_keys},
        {"legacy functional key matrix", test_legacy_functional_matrix},
        {"legacy UTF-8, Alt, and Escape", test_legacy_utf8_alt_and_escape},
        {"terminal responses are filtered", test_responses_and_control_strings_are_filtered},
        {"capability detection order", test_capability_detection_order},
        {"base-layout held alias", test_base_layout_held_alias},
        {"insufficient flags reject held state", test_insufficient_flags_never_claim_held_state},
        {"overlong sequence recovery", test_overlong_sequence_recovers},
        {"queue overflow preserves state", test_queue_overflow_keeps_state_correct},
        {"functional key names", test_functional_key_names},
        {"POSIX terminal lifecycle", test_posix_terminal_lifecycle}
    };
    size_t passed = 0u;
    size_t index;

    for (index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
