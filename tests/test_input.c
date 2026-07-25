#include "kitty_input.h"
#include "kitty_input_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/joystick.h>
#endif

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",             \
                          __FILE__, __LINE__, #condition);                     \
            return false;                                                     \
        }                                                                     \
    } while (false)

static void feed_fragmented(kittyin_input *input, const void *bytes,
                            size_t length)
{
    const unsigned char *source = bytes;
    size_t index;

    for (index = 0u; index < length; ++index)
        kittyin_input_feed(input, source + index, 1u);
}

static bool next_event(kittyin_input *input, kittyin_event *event,
                       kittyin_event_kind kind)
{
    CHECK(kittyin_input_next(input, event));
    CHECK(event->kind == kind);
    return true;
}

static bool test_wire_order_and_fragmentation(void)
{
    static const char bytes[] =
        "a"
        "\x1b[<0;11;7M"
        "\x1b[97;1:3u"
        "\x1b[I"
        "\x1b[200~paste \x1b[<0;1;1M \x1b[I!\x1b[201~"
        "\x1b[O";
    static const char pasted[] = "paste \x1b[<0;1;1M \x1b[I!";
    kittyin_input input;
    kittyin_event event;

    kittyin_input_init(&input);
    kittykb_input_set_terminal_flags(kittyin_input_keyboard(&input),
                                     KITTYKB_FLAGS_KEY_STATE);
    feed_fragmented(&input, bytes, sizeof bytes - 1u);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.key == (uint32_t)'a');
    CHECK(event.data.key.action == KITTYKB_ACTION_PRESS);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_MOUSE));
    CHECK(event.data.mouse.action == (uint8_t)KITTYIN_MOUSE_PRESS);
    CHECK(event.data.mouse.button == 1u);
    CHECK(event.data.mouse.x == 10 && event.data.mouse.y == 6);
    CHECK(event.data.mouse.pixel_coordinates);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.key == (uint32_t)'a');
    CHECK(event.data.key.action == KITTYKB_ACTION_RELEASE);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_FOCUS));
    CHECK(event.data.focus.focused);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_PASTE));
    CHECK(event.data.paste.final);
    CHECK(event.data.paste.length == sizeof pasted - 1u);
    CHECK(memcmp(event.data.paste.bytes, pasted, sizeof pasted - 1u) == 0);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_FOCUS));
    CHECK(!event.data.focus.focused);
    CHECK(!kittyin_input_next(&input, &event));
    CHECK(kittyin_input_get_stats(&input)->malformed_sequences == 0u);
    return true;
}

static bool test_mouse_matrix(void)
{
    static const char bytes[] =
        "\x1b[<20;3;4M"   /* ctrl + shift + button 1 */
        "\x1b[<32;5;6M"   /* button 1 motion */
        "\x1b[<0;5;6m"    /* button 1 release */
        "\x1b[<64;7;8M"   /* wheel up */
        "\x1b[<65;7;8M"   /* wheel down */
        "\x1b[<66;7;8M"   /* wheel left */
        "\x1b[<67;7;8M";  /* wheel right */
    kittyin_input input;
    kittyin_event event;
    int expected_wheel_x[] = {0, 0, -1, 1};
    int expected_wheel_y[] = {-1, 1, 0, 0};
    size_t index;

    kittyin_input_init(&input);
    kittyin_input_set_pixel_coordinates(&input, false);
    kittyin_input_feed(&input, bytes, sizeof bytes - 1u);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_MOUSE));
    CHECK(event.data.mouse.action == (uint8_t)KITTYIN_MOUSE_PRESS);
    CHECK(event.data.mouse.modifiers ==
          (KITTYKB_MOD_SHIFT | KITTYKB_MOD_CTRL));
    CHECK(!event.data.mouse.pixel_coordinates);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_MOUSE));
    CHECK(event.data.mouse.action == (uint8_t)KITTYIN_MOUSE_MOVE);
    CHECK(event.data.mouse.button == 1u);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_MOUSE));
    CHECK(event.data.mouse.action == (uint8_t)KITTYIN_MOUSE_RELEASE);
    CHECK(event.data.mouse.button == 1u);

    for (index = 0u; index < 4u; ++index) {
        CHECK(next_event(&input, &event, KITTYIN_EVENT_MOUSE));
        CHECK(event.data.mouse.action == (uint8_t)KITTYIN_MOUSE_WHEEL);
        CHECK(event.data.mouse.wheel_x == expected_wheel_x[index]);
        CHECK(event.data.mouse.wheel_y == expected_wheel_y[index]);
    }
    CHECK(!kittyin_input_next(&input, &event));
    return true;
}

static bool test_paste_chunking_and_marker_prefix(void)
{
    kittyin_input input;
    kittyin_event event;
    unsigned char long_paste[KITTYIN_PASTE_CHUNK_CAPACITY + 9u];
    size_t index;

    for (index = 0u; index < sizeof long_paste; ++index)
        long_paste[index] = (unsigned char)('a' + (index % 26u));
    kittyin_input_init(&input);
    feed_fragmented(&input, "\x1b[200~", sizeof("\x1b[200~") - 1u);
    kittyin_input_feed(&input, long_paste, sizeof long_paste);
    /* A failed terminator prefix must be emitted byte-for-byte. */
    feed_fragmented(&input, "\x1b[20x", sizeof("\x1b[20x") - 1u);
    feed_fragmented(&input, "\x1b[201~", sizeof("\x1b[201~") - 1u);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_PASTE));
    CHECK(!event.data.paste.final);
    CHECK(event.data.paste.length == KITTYIN_PASTE_CHUNK_CAPACITY);
    CHECK(memcmp(event.data.paste.bytes, long_paste,
                 KITTYIN_PASTE_CHUNK_CAPACITY) == 0);

    CHECK(next_event(&input, &event, KITTYIN_EVENT_PASTE));
    CHECK(event.data.paste.final);
    CHECK(event.data.paste.length == 14u);
    CHECK(memcmp(event.data.paste.bytes,
                 long_paste + KITTYIN_PASTE_CHUNK_CAPACITY, 9u) == 0);
    CHECK(memcmp(event.data.paste.bytes + 9u, "\x1b[20x", 5u) == 0);
    CHECK(!kittyin_input_next(&input, &event));
    CHECK(kittyin_input_get_stats(&input)->paste_chunks == 2u);
    return true;
}

static bool test_filtering_malformed_and_recovery(void)
{
    kittyin_input input;
    kittyin_event event;
    char digit = '1';
    size_t index;

    kittyin_input_init(&input);
    kittyin_input_feed(&input,
                       "\x1b]title\x07\x1bPignored\x1b\\"
                       "\x1b[<;1;2M"
                       "q",
                       sizeof("\x1b]title\x07\x1bPignored\x1b\\"
                              "\x1b[<;1;2M"
                              "q") - 1u);
    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.key == (uint32_t)'q');
    CHECK(!kittyin_input_next(&input, &event));
    CHECK(kittyin_input_get_stats(&input)->ignored_sequences == 2u);
    CHECK(kittyin_input_get_stats(&input)->malformed_sequences == 1u);

    kittyin_input_feed(&input, "\x1b[", 2u);
    for (index = 0u; index < KITTYIN_SEQUENCE_CAPACITY + 12u; ++index)
        kittyin_input_feed(&input, &digit, 1u);
    kittyin_input_feed(&input, "M", 1u);
    feed_fragmented(&input, "r", 1u);
    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.key == (uint32_t)'r');
    CHECK(kittyin_input_get_stats(&input)->malformed_sequences == 2u);
    return true;
}

static bool test_held_state_escape_and_external_binding(void)
{
    kittyin_input input;
    kittykb_input external;
    kittyin_event event;

    kittyin_input_init(&input);
    kittykb_input_init(&external);
    kittykb_input_set_terminal_flags(&external, KITTYKB_FLAGS_KEY_STATE);
    kittyin_input_bind_keyboard(&input, &external);
    feed_fragmented(&input, "\x1b[119;1:1u", sizeof("\x1b[119;1:1u") - 1u);
    CHECK(kittyin_input_has_release_events(&input));
    CHECK(kittyin_input_key_down(&input, (uint32_t)'w'));
    CHECK(kittyin_input_held_count(&input) == 1u);
    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.action == KITTYKB_ACTION_PRESS);

    feed_fragmented(&input, "\x1b[119;1:3u", sizeof("\x1b[119;1:3u") - 1u);
    CHECK(!kittyin_input_key_down(&input, (uint32_t)'w'));
    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.action == KITTYKB_ACTION_RELEASE);

    kittyin_input_feed(&input, "\x1b", 1u);
    CHECK(kittyin_input_has_pending_escape(&input));
    kittyin_input_flush_escape(&input);
    CHECK(next_event(&input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.key == KITTYKB_KEY_ESCAPE);
    return true;
}

static bool test_queue_overflow(void)
{
    kittyin_input input;
    kittyin_event event;
    size_t index;

    kittyin_input_init(&input);
    for (index = 0u; index < KITTYIN_EVENT_QUEUE_CAPACITY + 7u; ++index)
        kittyin_input_feed(&input, "x", 1u);
    CHECK(kittyin_input_get_stats(&input)->events_decoded ==
          KITTYIN_EVENT_QUEUE_CAPACITY + 7u);
    CHECK(kittyin_input_get_stats(&input)->dropped_events == 7u);
    for (index = 0u; index < KITTYIN_EVENT_QUEUE_CAPACITY; ++index)
        CHECK(kittyin_input_next(&input, &event));
    CHECK(!kittyin_input_next(&input, &event));
    return true;
}

static bool test_semantic_actions(void)
{
    kittyin_action_map map;
    kittyin_action_event actions[8];
    kittykb_event key = {0};
    kittyin_gamepad_event gamepad = {0};
    size_t count;

    kittyin_action_map_init(&map);
    CHECK(kittyin_action_map_bind_key(
        &map, 1u, (uint32_t)'w', 0u));
    CHECK(kittyin_action_map_bind_gamepad_button(
        &map, 1u, KITTYIN_GAMEPAD_ANY, 0u));
    CHECK(kittyin_action_map_bind_gamepad_axis(
        &map, 2u, KITTYIN_GAMEPAD_ANY, 1u, -1));
    CHECK(kittyin_action_map_has_binding(
        &map, 1u, KITTYIN_DEVICE_KEYBOARD));
    CHECK(kittyin_action_map_has_binding(
        &map, 1u, KITTYIN_DEVICE_GAMEPAD));
    CHECK(kittyin_action_map_set_repeat(&map, 320u, 85u));
    CHECK(kittyin_action_map_set_dead_zone(&map, 12000));

    key.key = (uint32_t)'w';
    key.action = KITTYKB_ACTION_PRESS;
    count = kittyin_action_map_key(&map, &key, actions, 8u);
    CHECK(count == 1u);
    CHECK(actions[0].action == 1u);
    CHECK(actions[0].phase == KITTYIN_ACTION_PRESS);
    CHECK(actions[0].device == KITTYIN_DEVICE_KEYBOARD);
    CHECK(kittyin_action_map_active_device(&map) ==
          KITTYIN_DEVICE_KEYBOARD);

    gamepad.kind = KITTYIN_GAMEPAD_CONNECTED;
    gamepad.gamepad = 0u;
    CHECK(kittyin_action_map_gamepad(
        &map, &gamepad, actions, 8u) == 0u);
    gamepad.kind = KITTYIN_GAMEPAD_AXIS;
    gamepad.control = 1u;
    gamepad.value = -11999;
    CHECK(kittyin_action_map_gamepad(
        &map, &gamepad, actions, 8u) == 0u);
    gamepad.value = -12000;
    count = kittyin_action_map_gamepad(&map, &gamepad, actions, 8u);
    CHECK(count == 1u);
    CHECK(actions[0].action == 2u);
    CHECK(actions[0].phase == KITTYIN_ACTION_PRESS);
    CHECK(actions[0].device == KITTYIN_DEVICE_GAMEPAD);
    CHECK(kittyin_action_map_active_gamepad(&map) == 0u);
    CHECK(kittyin_action_map_advance(&map, 319u, actions, 8u) == 0u);
    count = kittyin_action_map_advance(&map, 1u, actions, 8u);
    CHECK(count == 1u);
    CHECK(actions[0].phase == KITTYIN_ACTION_REPEAT);
    count = kittyin_action_map_advance(&map, 85u, actions, 8u);
    CHECK(count == 1u);
    CHECK(actions[0].phase == KITTYIN_ACTION_REPEAT);
    gamepad.value = 0;
    count = kittyin_action_map_gamepad(&map, &gamepad, actions, 8u);
    CHECK(count == 1u);
    CHECK(actions[0].phase == KITTYIN_ACTION_RELEASE);

    gamepad.kind = KITTYIN_GAMEPAD_BUTTON;
    gamepad.control = 0u;
    gamepad.value = 1;
    count = kittyin_action_map_gamepad(&map, &gamepad, actions, 8u);
    CHECK(count == 1u && actions[0].action == 1u);
    gamepad.value = 0;
    CHECK(kittyin_action_map_gamepad(
        &map, &gamepad, actions, 8u) == 1u);
    gamepad.kind = KITTYIN_GAMEPAD_DISCONNECTED;
    CHECK(kittyin_action_map_gamepad(
        &map, &gamepad, actions, 8u) == 0u);
    CHECK(kittyin_action_map_active_device(&map) ==
          KITTYIN_DEVICE_KEYBOARD);

    CHECK(kittyin_action_map_unbind(
        &map, 1u, KITTYIN_DEVICE_KEYBOARD) == 1u);
    CHECK(!kittyin_action_map_has_binding(
        &map, 1u, KITTYIN_DEVICE_KEYBOARD));
    CHECK(kittyin_action_map_bind_key(
        &map, 1u, (uint32_t)'k', 0u));
    key.key = (uint32_t)'w';
    CHECK(kittyin_action_map_key(&map, &key, actions, 8u) == 0u);
    key.key = (uint32_t)'k';
    CHECK(kittyin_action_map_key(&map, &key, actions, 8u) == 1u);
    return true;
}

static bool test_gamepad_lifecycle(void)
{
#ifdef __linux__
    kittyin_gamepads gamepads;
    kittyin_gamepad_event event;
    struct js_event source = {0};
    int descriptors[2] = {-1, -1};

    CHECK(pipe(descriptors) == 0);
    kittyin_gamepads_init(&gamepads);
    CHECK(kittyin_gamepads_set_directory(
        &gamepads, "/path-that-does-not-exist"));
    CHECK(kittyin_gamepads_set_scan_interval(&gamepads, 10u));
    CHECK(kittyin_gamepads_attach_fd(
        &gamepads, 0u, descriptors[0], "Test Pad", 8u, 11u));
    descriptors[0] = -1;
    CHECK(kittyin_gamepads_next(&gamepads, &event));
    CHECK(event.kind == KITTYIN_GAMEPAD_CONNECTED);
    CHECK(event.gamepad == 0u);

    source.type = JS_EVENT_BUTTON;
    source.number = 2u;
    source.value = 1;
    CHECK(write(descriptors[1], &source, sizeof source) ==
          (ssize_t)sizeof source);
    source.type = (uint8_t)(JS_EVENT_AXIS | JS_EVENT_INIT);
    source.number = 3u;
    source.value = -12345;
    CHECK(write(descriptors[1], &source, sizeof source) ==
          (ssize_t)sizeof source);
    CHECK(close(descriptors[1]) == 0);
    descriptors[1] = -1;
    kittyin_gamepads_poll(&gamepads, 10);

    CHECK(kittyin_gamepads_next(&gamepads, &event));
    CHECK(event.kind == KITTYIN_GAMEPAD_BUTTON);
    CHECK(event.control == 2u && event.value == 1);
    CHECK(!event.initial);
    CHECK(kittyin_gamepads_next(&gamepads, &event));
    CHECK(event.kind == KITTYIN_GAMEPAD_AXIS);
    CHECK(event.control == 3u && event.value == -12345);
    CHECK(event.initial);
    CHECK(kittyin_gamepads_next(&gamepads, &event));
    CHECK(event.kind == KITTYIN_GAMEPAD_DISCONNECTED);
    CHECK(!kittyin_gamepads_next(&gamepads, &event));
    CHECK(kittyin_gamepads_dropped_events(&gamepads) == 0u);
    kittyin_gamepads_close(&gamepads);
#endif
    return true;
}

static bool contains_bytes(const unsigned char *haystack, size_t haystack_size,
                           const char *needle)
{
    const size_t needle_size = strlen(needle);
    size_t index;

    if (needle_size > haystack_size) return false;
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) return true;
    }
    return false;
}

static ssize_t read_available(int fd, unsigned char *bytes, size_t capacity)
{
    size_t total = 0u;

    while (total < capacity) {
        struct pollfd descriptor;
        ssize_t count;
        int result;

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        do {
            result = poll(&descriptor, 1u, 20);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || (descriptor.revents & POLLIN) == 0) break;
        count = read(fd, bytes + total, capacity - total);
        if (count > 0) total += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    return (ssize_t)total;
}

static bool test_posix_lifecycle(void)
{
    static const char incoming[] = "k\x1b[<0;4;5M\x1b[I";
    kittyin_terminal terminal;
    kittyin_terminal_options options;
    kittyin_event event;
    unsigned char controls[512];
    ssize_t control_count;
    int master = -1;
    int slave = -1;

    CHECK(openpty(&master, &slave, NULL, NULL, NULL) == 0);
    kittyin_terminal_init(&terminal);
    kittyin_terminal_options_init(&options);
    options.keyboard.probe_timeout_ms = 0;
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;
    CHECK(kittyin_terminal_start(&terminal, slave, slave, &options) == 0);
    control_count = read_available(master, controls, sizeof controls);
    CHECK(control_count > 0);
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[>31u"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?1003h"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?1006h"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?1016h"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?1004h"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?2004h"));

    CHECK(write(master, incoming, sizeof incoming - 1u) ==
          (ssize_t)(sizeof incoming - 1u));
    CHECK(kittyin_terminal_read(&terminal) == (int)(sizeof incoming - 1u));
    CHECK(next_event(&terminal.input, &event, KITTYIN_EVENT_KEY));
    CHECK(event.data.key.key == (uint32_t)'k');
    CHECK(next_event(&terminal.input, &event, KITTYIN_EVENT_MOUSE));
    CHECK(event.data.mouse.x == 3 && event.data.mouse.y == 4);
    CHECK(next_event(&terminal.input, &event, KITTYIN_EVENT_FOCUS));
    CHECK(event.data.focus.focused);

    CHECK(kittyin_terminal_stop(&terminal) == 0);
    control_count = read_available(master, controls, sizeof controls);
    CHECK(control_count > 0);
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?2004l"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?1003l"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[<u"));
    CHECK(!terminal.active);

    /* An emergency restore emits the selected controls once.  Normal stop
     * still completes cleanup, and the same object can be started again. */
    CHECK(kittyin_terminal_start(&terminal, slave, slave, &options) == 0);
    (void)read_available(master, controls, sizeof controls);
    kittyin_terminal_emergency_restore(&terminal);
    control_count = read_available(master, controls, sizeof controls);
    CHECK(control_count > 0);
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?2004l"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[?1003l"));
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[<u"));
    CHECK(kittyin_terminal_stop(&terminal) == 0);
    CHECK(read_available(master, controls, sizeof controls) == 0);

    /* Modes left off by the caller must not be disabled on shutdown. */
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_OFF;
    options.focus_events = false;
    options.bracketed_paste = false;
    CHECK(kittyin_terminal_start(&terminal, slave, slave, &options) == 0);
    (void)read_available(master, controls, sizeof controls);
    CHECK(kittyin_terminal_stop(&terminal) == 0);
    control_count = read_available(master, controls, sizeof controls);
    CHECK(control_count > 0);
    CHECK(contains_bytes(controls, (size_t)control_count, "\x1b[<u"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?2004l"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?1004l"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?1016l"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?1006l"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?1000l"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?1002l"));
    CHECK(!contains_bytes(controls, (size_t)control_count, "\x1b[?1003l"));
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"wire order and fragmentation", test_wire_order_and_fragmentation},
        {"mouse matrix", test_mouse_matrix},
        {"paste chunking", test_paste_chunking_and_marker_prefix},
        {"filtering and recovery", test_filtering_malformed_and_recovery},
        {"held state and escape", test_held_state_escape_and_external_binding},
        {"queue overflow", test_queue_overflow},
        {"semantic actions", test_semantic_actions},
        {"gamepad lifecycle", test_gamepad_lifecycle},
        {"POSIX lifecycle", test_posix_lifecycle}
    };
    size_t index;

    for (index = 0u; index < sizeof tests / sizeof tests[0]; ++index) {
        if (!tests[index].run()) {
            (void)fprintf(stderr, "FAIL: %s\n", tests[index].name);
            return 1;
        }
        (void)printf("ok: %s\n", tests[index].name);
    }
    return 0;
}
